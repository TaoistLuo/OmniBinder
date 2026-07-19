#include <gtest/gtest.h>
#include "transport/shm_transport.h"
#include "platform/platform.h"
#include "core/event_loop.h"
#include "core/topic_runtime.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include <memory>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace omnibinder;

// RAII helper: runs a background thread that accepts handshake connections
// on the server's listener and completes each opaque handle exchange.
// Client connect() blocks until the server accepts and sends notification handles.
class HandshakeHandlerThread {
public:
    explicit HandshakeHandlerThread(ShmTransport& server)
        : stop_(false)
    {
        thread_ = std::thread([this, &server]() {
            int listen_fd = server.handshakeListenFd();
            while (!stop_.load()) {
                if (platform::waitFdReadable(listen_fd, 10)) {
                    server.onHandshakeClientConnect();
                }
            }
        });
    }

    ~HandshakeHandlerThread() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::atomic<bool> stop_;
    std::thread thread_;
};

class ShmTransportTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { platform::netInit(); }
    static void TearDownTestSuite() { platform::netCleanup(); }
};

class ShmLifecycleOwner {
public:
    explicit ShmLifecycleOwner(ShmTransport& server)
        : server_(server), ready_(false), cleanup_count_(0), timer_count_(0),
          wrong_thread_(false)
    {
        thread_ = std::thread([this]() {
            owner_thread_ = std::this_thread::get_id();
            server_.setOnClientConnected(
                [this](uint32_t client_id, int liveness_fd, int notify_fd) {
                    loop_.addFd(liveness_fd,
                        EventLoop::EVENT_READ | EventLoop::EVENT_ERROR,
                        [this, client_id, notify_fd](int fd, uint32_t) {
                            if (std::this_thread::get_id() != owner_thread_) {
                                wrong_thread_.store(true);
                            }
                            loop_.removeFd(fd);
                            if (notify_fd >= 0) loop_.removeFd(notify_fd);
                            if (server_.removeClient(client_id)) ++cleanup_count_;
                        });
                    if (notify_fd >= 0) {
                        loop_.addFd(notify_fd, EventLoop::EVENT_READ,
                            [](int fd, uint32_t) { platform::eventFdConsume(fd); });
                    }
                });
            loop_.addFd(server_.handshakeListenFd(), EventLoop::EVENT_READ,
                [this](int, uint32_t) { server_.onHandshakeClientConnect(); });
            loop_.addTimer(20, [this]() { ++timer_count_; }, true);
            ready_.store(true);
            loop_.run();
        });
        while (!ready_.load()) std::this_thread::yield();
    }

    ~ShmLifecycleOwner()
    {
        loop_.stop();
        if (thread_.joinable()) thread_.join();
    }

    bool waitForClientCount(uint32_t expected, uint32_t timeout_ms = 1000)
    {
        for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
            if (server_.clientCount() == expected) return true;
            platform::sleepMs(1);
        }
        return server_.clientCount() == expected;
    }

    uint32_t cleanupCount() const { return cleanup_count_.load(); }
    uint32_t timerCount() const { return timer_count_.load(); }
    bool waitForCleanupCount(uint32_t expected, uint32_t timeout_ms = 1000)
    {
        for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
            if (cleanup_count_.load() == expected) return true;
            platform::sleepMs(1);
        }
        return cleanup_count_.load() == expected;
    }
    bool cleanupRanOnOwnerThread() const { return !wrong_thread_.load(); }

private:
    ShmTransport& server_;
    EventLoop loop_;
    std::thread thread_;
    std::thread::id owner_thread_;
    std::atomic<bool> ready_;
    std::atomic<uint32_t> cleanup_count_;
    std::atomic<uint32_t> timer_count_;
    std::atomic<bool> wrong_thread_;
};

#ifndef _WIN32
namespace {

const uint32_t TEST_HANDSHAKE_MAGIC = 0x484E4453u;

struct TestHandshakeHeader {
    uint32_t magic;
    uint32_t payload_len;
    uint32_t fd_count;
};

std::string listenerPath(int listener_fd)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t addr_len = sizeof(addr);
    if (getsockname(listener_fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) != 0) {
        return std::string();
    }
    return std::string(addr.sun_path);
}

int connectRawHandshakePeer(int listener_fd)
{
    std::string path = listenerPath(listener_fd);
    if (path.empty()) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool sendRawBytes(int fd, const void* data, size_t length)
{
    const char* bytes = static_cast<const char*>(data);
    while (length > 0) {
        ssize_t sent = send(fd, bytes, length, MSG_NOSIGNAL);
        if (sent > 0) {
            bytes += sent;
            length -= static_cast<size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool sendRawHeaderWithFd(int fd, const TestHandshakeHeader& header, int sent_fd)
{
    struct iovec iov;
    iov.iov_base = const_cast<TestHandshakeHeader*>(&header);
    iov.iov_len = sizeof(header);
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &sent_fd, sizeof(sent_fd));
    ssize_t result;
    do {
        result = sendmsg(fd, &msg, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    return result == static_cast<ssize_t>(sizeof(header));
}

class LinuxHandshakeFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        static std::atomic<uint32_t> sequence(0);
        std::ostringstream path;
        path << "/tmp/omni_handshake_test_" << getpid() << "_" << sequence++ << ".sock";
        listener_ = platform::handshakeListen(path.str());
        ASSERT_NE(listener_, static_cast<platform::handshake_listener*>(NULL));
    }

    void TearDown() override
    {
        platform::handshakeCloseListener(listener_);
    }

    platform::handshake_channel* acceptRawPeer(int* peer_fd)
    {
        *peer_fd = connectRawHandshakePeer(platform::handshakeGetListenerFd(listener_));
        if (*peer_fd < 0) return NULL;
        return platform::handshakeAccept(listener_);
    }

    bool receive(platform::handshake_channel* channel)
    {
        char payload[32];
        size_t payload_len = 0;
        int fd_count = 0;
        return platform::handshakeRecv(channel, payload, sizeof(payload), &payload_len,
                                       NULL, 0, &fd_count);
    }

    platform::handshake_listener* listener_;
};

TEST_F(LinuxHandshakeFixture, NoBytesTimesOutAndAcceptedFdIsNonblocking)
{
    int peer_fd = -1;
    platform::handshake_channel* channel = acceptRawPeer(&peer_fd);
    ASSERT_NE(channel, static_cast<platform::handshake_channel*>(NULL));
    int flags = fcntl(platform::handshakeGetFd(channel), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);
    int64_t started = platform::currentTimeMs();
    EXPECT_FALSE(receive(channel));
    EXPECT_GE(platform::currentTimeMs() - started, 300);
    EXPECT_LT(platform::currentTimeMs() - started, 1000);
    close(peer_fd);
    platform::handshakeClose(channel);
}

TEST_F(LinuxHandshakeFixture, PartialHeaderUsesOriginalDeadline)
{
    int peer_fd = -1;
    platform::handshake_channel* channel = acceptRawPeer(&peer_fd);
    ASSERT_NE(channel, static_cast<platform::handshake_channel*>(NULL));
    TestHandshakeHeader header = {TEST_HANDSHAKE_MAGIC, 0, 0};
    ASSERT_TRUE(sendRawBytes(peer_fd, &header, sizeof(header) - 1));
    platform::sleepMs(300);
    int64_t started = platform::currentTimeMs();
    EXPECT_FALSE(receive(channel));
    EXPECT_LT(platform::currentTimeMs() - started, 400);
    close(peer_fd);
    platform::handshakeClose(channel);
}

TEST_F(LinuxHandshakeFixture, CompleteHeaderAndPartialPayloadTimesOut)
{
    int peer_fd = -1;
    platform::handshake_channel* channel = acceptRawPeer(&peer_fd);
    ASSERT_NE(channel, static_cast<platform::handshake_channel*>(NULL));
    TestHandshakeHeader header = {TEST_HANDSHAKE_MAGIC, 8, 0};
    ASSERT_TRUE(sendRawBytes(peer_fd, &header, sizeof(header)));
    const char partial[] = "part";
    ASSERT_TRUE(sendRawBytes(peer_fd, partial, sizeof(partial) - 1));
    int64_t started = platform::currentTimeMs();
    EXPECT_FALSE(receive(channel));
    EXPECT_LT(platform::currentTimeMs() - started, 1000);
    close(peer_fd);
    platform::handshakeClose(channel);
}

TEST_F(LinuxHandshakeFixture, EofFailsImmediately)
{
    int peer_fd = -1;
    platform::handshake_channel* channel = acceptRawPeer(&peer_fd);
    ASSERT_NE(channel, static_cast<platform::handshake_channel*>(NULL));
    close(peer_fd);
    int64_t started = platform::currentTimeMs();
    EXPECT_FALSE(receive(channel));
    EXPECT_LT(platform::currentTimeMs() - started, 100);
    platform::handshakeClose(channel);
}

TEST_F(LinuxHandshakeFixture, MalformedFrameClosesReceivedRights)
{
    int peer_fd = -1;
    platform::handshake_channel* channel = acceptRawPeer(&peer_fd);
    ASSERT_NE(channel, static_cast<platform::handshake_channel*>(NULL));
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);
    TestHandshakeHeader header = {0, 0, 1};
    ASSERT_TRUE(sendRawHeaderWithFd(peer_fd, header, pipe_fds[0]));
    close(pipe_fds[0]);
    EXPECT_FALSE(receive(channel));
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = pipe_fds[1];
    pfd.events = POLLOUT;
    ASSERT_EQ(poll(&pfd, 1, 100), 1);
    EXPECT_NE(pfd.revents & POLLERR, 0);
    close(pipe_fds[1]);
    close(peer_fd);
    platform::handshakeClose(channel);
}

} // namespace
#endif

#ifndef _WIN32
TEST_F(ShmTransportTest, StalledHandshakeOnlyBoundsOwnerLoopOnce)
{
    std::string shm_name = generateShmName("stalled_owner_loop_test");
    ShmTransport server(shm_name, true);
    ShmLifecycleOwner owner(server);
    uint32_t timer_before = owner.timerCount();
    int peer_fd = connectRawHandshakePeer(server.handshakeListenFd());
    ASSERT_GE(peer_fd, 0);

    platform::sleepMs(750);
    EXPECT_GT(owner.timerCount(), timer_before);
    EXPECT_EQ(server.clientCount(), 0u);

    ShmTransport healthy_client(shm_name, false);
    ASSERT_EQ(healthy_client.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));
    healthy_client.close();
    ASSERT_TRUE(owner.waitForClientCount(0));
    close(peer_fd);
}

TEST_F(ShmTransportTest, UnexpectedReadableHandshakeDataUsesLivenessCleanup)
{
    std::string server_name = generateShmName("readable_liveness_test");
    ShmTransport server(server_name, true);
    ShmLifecycleOwner owner(server);

    std::ostringstream client_name_builder;
    client_name_builder << "omni_readable_client_" << getpid();
    std::string client_name = client_name_builder.str();
    const uint32_t capacity = static_cast<uint32_t>(SHM_DEFAULT_REQ_RING_CAPACITY);
    size_t shm_size = calculateShmSize(capacity, capacity, 1);
    void* addr = platform::shmCreate(client_name, shm_size, true);
    ASSERT_NE(addr, static_cast<void*>(NULL));
    memset(addr, 0, shm_size);
    ShmControlBlock* ctrl = static_cast<ShmControlBlock*>(addr);
    ctrl->magic = SHM_MAGIC;
    ctrl->version = 1;
    ctrl->req_ring_capacity = capacity;
    ctrl->resp_ring_capacity = capacity;
    ctrl->ready_flag = 1;
    size_t req_offset = (sizeof(ShmControlBlock) + alignof(ShmRingHeader) - 1)
        / alignof(ShmRingHeader) * alignof(ShmRingHeader);
    ShmRingHeader* req_ring = reinterpret_cast<ShmRingHeader*>(
        static_cast<uint8_t*>(addr) + req_offset);
    req_ring->capacity = capacity;
    size_t resp_offset = req_offset + sizeof(ShmRingHeader) + capacity;
    resp_offset = (resp_offset + alignof(ShmRingHeader) - 1)
        / alignof(ShmRingHeader) * alignof(ShmRingHeader);
    ShmRingHeader* resp_ring = reinterpret_cast<ShmRingHeader*>(
        static_cast<uint8_t*>(addr) + resp_offset);
    resp_ring->capacity = capacity;

    platform::handshake_channel* client_channel = platform::handshakeConnect(
        listenerPath(server.handshakeListenFd()));
    ASSERT_NE(client_channel, static_cast<platform::handshake_channel*>(NULL));
    ASSERT_TRUE(platform::handshakeSend(client_channel, client_name.data(),
                                        client_name.size(), NULL, 0));
    int received_fds[2] = {-1, -1};
    size_t response_len = 0;
    int received_count = 0;
    ASSERT_TRUE(platform::handshakeRecv(client_channel, NULL, 0, &response_len,
                                        received_fds, 2, &received_count));
    ASSERT_EQ(response_len, 0u);
    ASSERT_EQ(received_count, 2);
    ASSERT_TRUE(owner.waitForClientCount(1));
    for (int i = 0; i < received_count; ++i) platform::closeEventFd(received_fds[i]);

    ASSERT_EQ(send(platform::handshakeGetFd(client_channel), "x", 1, MSG_NOSIGNAL), 1);
    ASSERT_TRUE(owner.waitForCleanupCount(1));
    ASSERT_TRUE(owner.waitForClientCount(0));

    platform::handshakeClose(client_channel);
    platform::shmDetach(addr, shm_size);
    platform::shmUnlink(client_name);
}
#endif

TEST_F(ShmTransportTest, CalculateShmSize) {
    EXPECT_GE(calculateShmSize(1024, 512),
              sizeof(ShmControlBlock)
              + sizeof(ShmRingHeader) + 1024
              + sizeof(ShmRingHeader) + 512);
    EXPECT_EQ(sizeof(ShmControlBlock) % alignof(ShmRingHeader), 0u);
    EXPECT_EQ(sizeof(ShmRingHeader) % alignof(ShmRingHeader), 0u);
}

TEST_F(ShmTransportTest, CalculateShmSizeNormalizesTinyRings) {
    EXPECT_EQ(calculateShmSize(1, 1), calculateShmSize(64, 64));
    EXPECT_GT(calculateShmSize(1, 1), 0u);
}

TEST_F(ShmTransportTest, GenerateShmName) {
    EXPECT_EQ(generateShmName("myservice"), "/binder_myservice_cfb2a296");
    EXPECT_EQ(generateShmName("test"), "/binder_test_afd071e5");
    EXPECT_NE(generateShmName("test"), generateShmName("Test"));
}

TEST_F(ShmTransportTest, ServerCreate) {
    std::string shm_name = generateShmName("srv_create_test");
    ShmTransport server(shm_name, true);
    EXPECT_EQ(server.state(), ConnectionState::CONNECTED);
    EXPECT_TRUE(server.isServer());
    EXPECT_EQ(server.type(), TransportType::SHM);
    EXPECT_GE(server.fd(), 0);
    EXPECT_EQ(server.clientCount(), 0);
    server.close();
}

TEST_F(ShmTransportTest, ClientConnect) {
    std::string shm_name = generateShmName("client_conn_test");
    ShmTransport server(shm_name, true);
    ASSERT_EQ(server.state(), ConnectionState::CONNECTED);
    HandshakeHandlerThread handshake_handler(server);

    ShmTransport client(shm_name, false);
    EXPECT_EQ(client.state(), ConnectionState::DISCONNECTED);

    ASSERT_EQ(client.connect("", 0), 0);
    EXPECT_EQ(client.state(), ConnectionState::CONNECTED);

    // Wait for HandshakeHandlerThread to finish the server-side handshake.
    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) {
        platform::sleepMs(1);
    }
    EXPECT_EQ(server.clientCount(), 1);

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, MultipleClientsConnect) {
    std::string shm_name = generateShmName("multi_client_test");
    ShmTransport server(shm_name, true);
    HandshakeHandlerThread handshake_handler(server);

    ShmTransport client0(shm_name, false);
    ASSERT_EQ(client0.connect("", 0), 0);

    ShmTransport client1(shm_name, false);
    ASSERT_EQ(client1.connect("", 0), 0);

    ShmTransport client2(shm_name, false);
    ASSERT_EQ(client2.connect("", 0), 0);

    // Wait for HandshakeHandlerThread to process all handshakes.
    for (int i = 0; i < 50 && server.clientCount() < 3u; ++i) {
        platform::sleepMs(1);
    }
    EXPECT_EQ(server.clientCount(), 3u);

    client0.close();
    // Server detects disconnection asynchronously

    client1.close();
    client2.close();
    server.close();
}

TEST_F(ShmTransportTest, CleanCloseReclaimsExactlyOnceOnOwnerLoop) {
    std::string shm_name = generateShmName("clean_liveness_test");
    ShmTransport server(shm_name, true);
    ShmLifecycleOwner owner(server);
    ShmTransport client(shm_name, false);

    ASSERT_EQ(client.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));
    client.close();
    ASSERT_TRUE(owner.waitForCleanupCount(1));
    ASSERT_TRUE(owner.waitForClientCount(0));
    EXPECT_EQ(owner.cleanupCount(), 1u);
    EXPECT_TRUE(owner.cleanupRanOnOwnerThread());
    platform::sleepMs(10);
    EXPECT_EQ(owner.cleanupCount(), 1u);
}

TEST_F(ShmTransportTest, ClosedClientDoesNotHarmSurvivingClient) {
    std::string shm_name = generateShmName("surviving_client_test");
    ShmTransport server(shm_name, true);
    ShmLifecycleOwner owner(server);
    ShmTransport departing(shm_name, false);
    ShmTransport survivor(shm_name, false);

    ASSERT_EQ(departing.connect("", 0), 0);
    ASSERT_EQ(survivor.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(2));
    std::vector<uint32_t> ids = server.activeClientIds();
    ASSERT_EQ(ids.size(), 2u);

    departing.close();
    ASSERT_TRUE(owner.waitForCleanupCount(1));
    ASSERT_TRUE(owner.waitForClientCount(1));
    std::vector<uint32_t> surviving_ids = server.activeClientIds();
    ASSERT_EQ(surviving_ids.size(), 1u);
    const uint8_t response[] = {9, 8, 7};
    ASSERT_EQ(server.serverSend(surviving_ids[0], response, sizeof(response)),
              static_cast<int>(sizeof(response)));
    ASSERT_TRUE(platform::waitFdReadable(survivor.fd(), 100));
    uint8_t received[sizeof(response)] = {0};
    EXPECT_EQ(survivor.recv(received, sizeof(received)), static_cast<int>(sizeof(received)));
    EXPECT_EQ(memcmp(received, response, sizeof(response)), 0);
    EXPECT_EQ(owner.cleanupCount(), 1u);
}

#ifndef _WIN32
TEST_F(ShmTransportTest, AbruptPeerDeathReclaimsAndAllowsReconnect) {
    std::string shm_name = generateShmName("abrupt_liveness_test");
    ShmTransport server(shm_name, true);
    ShmLifecycleOwner owner(server);

    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ShmTransport client(shm_name, false);
        int result = client.connect("", 0);
        _exit(result == 0 ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    ASSERT_TRUE(owner.waitForCleanupCount(1));
    ASSERT_TRUE(owner.waitForClientCount(0));
    EXPECT_EQ(owner.cleanupCount(), 1u);

    ShmTransport reconnected(shm_name, false);
    ASSERT_EQ(reconnected.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));
    EXPECT_EQ(server.activeClientIds().size(), 1u);
    reconnected.close();
    ASSERT_TRUE(owner.waitForClientCount(0));
    ASSERT_TRUE(owner.waitForCleanupCount(2));
    EXPECT_EQ(owner.cleanupCount(), 2u);
    EXPECT_TRUE(owner.cleanupRanOnOwnerThread());
}
#endif

TEST(TopicRuntimeTest, ExactShmRemovalPreservesOtherClientsAndServices) {
    TopicRuntime topics;
    const uint32_t first_topic = 11;
    const uint32_t second_topic = 22;
    topics.addShmSubscriberService(first_topic, "service-a", 1);
    topics.addShmSubscriberService(first_topic, "service-a", 2);
    topics.addShmSubscriberService(first_topic, "service-b", 1);
    topics.addShmSubscriberService(second_topic, "service-a", 1);
    topics.addShmSubscriberService(second_topic, "service-a", 3);

    topics.removeShmSubscriberService("service-a", 1);

    ASSERT_EQ(topics.shmSubscribers(first_topic).size(), 2u);
    EXPECT_EQ(topics.shmSubscribers(first_topic)[0].client_id, 2u);
    EXPECT_EQ(topics.shmSubscribers(first_topic)[1].service_name, "service-b");
    ASSERT_EQ(topics.shmSubscribers(second_topic).size(), 1u);
    EXPECT_EQ(topics.shmSubscribers(second_topic)[0].client_id, 3u);
}

TEST_F(ShmTransportTest, ActiveResponseArenaStats) {
    std::string shm_name = generateShmName("arena_stats_test");
    ShmTransport server(shm_name, true);
    HandshakeHandlerThread handshake_handler(server);

    EXPECT_GE(server.clientCount(), 0u);

    ShmTransport client0(shm_name, false);
    ASSERT_EQ(client0.connect("", 0), 0);
    // Wait for the server-side handshake.
    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) platform::sleepMs(1);
    EXPECT_EQ(server.clientCount(), 1u);

    ShmTransport client1(shm_name, false);
    ASSERT_EQ(client1.connect("", 0), 0);
    for (int i = 0; i < 50 && server.clientCount() < 2u; ++i) platform::sleepMs(1);
    EXPECT_EQ(server.clientCount(), 2u);

    client0.close();
    client1.close();
    server.close();
}

TEST_F(ShmTransportTest, ClientSendServerRecv) {
    std::string shm_name = generateShmName("c2s_test");
    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false, 128 * 1024, 128 * 1024);
    client.connect("", 0);

    const size_t data_size = 64 * 1024;
    std::vector<uint8_t> send_data(data_size);
    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    ASSERT_EQ(client.send(send_data.data(), data_size), static_cast<int>(data_size));
    platform::sleepMs(20);

    std::vector<uint8_t> recv_data(data_size);
    uint32_t from_id = 0;
    ASSERT_EQ(server.serverRecv(recv_data.data(), data_size, from_id), static_cast<int>(data_size));
    EXPECT_GT(from_id, 0u);
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    ASSERT_EQ(server.serverSend(from_id, send_data.data(), data_size), static_cast<int>(data_size));
    platform::sleepMs(20);

    memset(recv_data.data(), 0, data_size);
    ASSERT_EQ(client.recv(recv_data.data(), data_size), static_cast<int>(data_size));
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, Delivers65537ByteFramesBothDirections) {
    const size_t data_size = 65537;
    const size_t ring_size = 128 * 1024;
    std::string shm_name = generateShmName("frame_65537_test");
    ShmTransport server(shm_name, true, ring_size, ring_size);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false, ring_size, ring_size);
    ASSERT_EQ(client.connect("", 0), 0);

    std::vector<uint8_t> request(data_size);
    for (size_t i = 0; i < request.size(); ++i) request[i] = static_cast<uint8_t>(i * 31u + 7u);
    ASSERT_EQ(client.send(request.data(), request.size()), static_cast<int>(request.size()));

    size_t next_size = 0;
    uint32_t client_id = 0;
    ASSERT_EQ(server.nextServerRecvSize(next_size, client_id), 1);
    ASSERT_EQ(next_size, request.size());
    std::vector<uint8_t> received(next_size);
    ASSERT_EQ(server.serverRecv(received.data(), received.size(), client_id),
              static_cast<int>(request.size()));
    EXPECT_EQ(received, request);

    std::vector<uint8_t> response(data_size);
    for (size_t i = 0; i < response.size(); ++i) response[i] = static_cast<uint8_t>(i * 17u + 3u);
    ASSERT_EQ(server.serverSend(client_id, response.data(), response.size()),
              static_cast<int>(response.size()));
    ASSERT_EQ(client.nextRecvSize(next_size), 1);
    ASSERT_EQ(next_size, response.size());
    received.resize(next_size);
    ASSERT_EQ(client.recv(received.data(), received.size()), static_cast<int>(response.size()));
    EXPECT_EQ(received, response);
}

TEST_F(ShmTransportTest, LargerConfiguredRingPreservesPayloadAndLargeThenSmallOrder) {
    const size_t large_size = 100 * 1024;
    const size_t ring_size = 256 * 1024;
    std::string shm_name = generateShmName("large_then_small_test");
    ShmTransport server(shm_name, true, ring_size, ring_size);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false, ring_size, ring_size);
    ASSERT_EQ(client.connect("", 0), 0);

    std::vector<uint8_t> large(large_size);
    for (size_t i = 0; i < large.size(); ++i) large[i] = static_cast<uint8_t>((i ^ (i >> 8)) & 0xffu);
    const uint8_t small[] = {0xde, 0xad, 0xbe, 0xef};
    ASSERT_EQ(client.send(large.data(), large.size()), static_cast<int>(large.size()));
    ASSERT_EQ(client.send(small, sizeof(small)), static_cast<int>(sizeof(small)));

    size_t next_size = 0;
    uint32_t client_id = 0;
    ASSERT_EQ(server.nextServerRecvSize(next_size, client_id), 1);
    ASSERT_EQ(next_size, large.size());
    std::vector<uint8_t> received(next_size);
    ASSERT_EQ(server.serverRecv(received.data(), received.size(), client_id),
              static_cast<int>(large.size()));
    EXPECT_EQ(received, large);
    ASSERT_EQ(server.nextServerRecvSize(next_size, client_id), 1);
    ASSERT_EQ(next_size, sizeof(small));
    received.resize(next_size);
    ASSERT_EQ(server.serverRecv(received.data(), received.size(), client_id),
              static_cast<int>(sizeof(small)));
    EXPECT_EQ(memcmp(received.data(), small, sizeof(small)), 0);
}

#ifndef _WIN32
TEST_F(ShmTransportTest, ImpossibleHeadDeterministicallyDisconnectsClient) {
    const uint32_t ring_size = 128 * 1024;
    std::string shm_name = generateShmName("invalid_head_disconnect_test");
    ShmTransport server(shm_name, true, ring_size, ring_size);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false, ring_size, ring_size);
    std::atomic<uint32_t> disconnect_reports(0);
    server.setOnClientDisconnected(
        [&server, &disconnect_reports](uint32_t client_id, int, int) {
            ++disconnect_reports;
            if (!server.removeClient(client_id)) {
                ADD_FAILURE() << "malformed frame cleanup was not exactly once";
            }
        });
    ASSERT_EQ(client.connect("", 0), 0);
    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) platform::sleepMs(1);
    ASSERT_EQ(server.clientCount(), 1u);

    size_t mapped_size = 0;
    void* addr = platform::shmCreate(client.shmName(), 1, false, &mapped_size);
    ASSERT_NE(addr, static_cast<void*>(NULL));
    ASSERT_GE(mapped_size, calculateShmSize(ring_size, ring_size));
    size_t req_offset = (sizeof(ShmControlBlock) + alignof(ShmRingHeader) - 1)
        / alignof(ShmRingHeader) * alignof(ShmRingHeader);
    ShmRingHeader* req_ring = reinterpret_cast<ShmRingHeader*>(
        static_cast<uint8_t*>(addr) + req_offset);
    uint8_t* req_data = reinterpret_cast<uint8_t*>(req_ring + 1);
    uint32_t impossible_length = ring_size;
    memcpy(req_data, &impossible_length, sizeof(impossible_length));
    req_ring->read_pos.store(0, std::memory_order_release);
    req_ring->write_pos.store(sizeof(impossible_length), std::memory_order_release);

    size_t next_size = 0;
    uint32_t client_id = 0;
    EXPECT_EQ(server.nextServerRecvSize(next_size, client_id), -1);
    EXPECT_EQ(disconnect_reports.load(), 1u);
    EXPECT_EQ(server.clientCount(), 0u);
    EXPECT_EQ(server.nextServerRecvSize(next_size, client_id), 0);
    EXPECT_EQ(disconnect_reports.load(), 1u);
    platform::shmDetach(addr, mapped_size);
}

TEST_F(ShmTransportTest, RepeatedLargeObjectOpenReportsActualDetachLength) {
    std::ostringstream name_builder;
    name_builder << "omni_map_length_" << getpid();
    const std::string name = name_builder.str();
    const size_t object_size = calculateShmSize(128 * 1024, 128 * 1024);
    size_t created_size = 0;
    void* owner = platform::shmCreate(name, object_size, true, &created_size);
    ASSERT_NE(owner, static_cast<void*>(NULL));
    ASSERT_EQ(created_size, object_size);

    for (int i = 0; i < 100; ++i) {
        size_t mapped_size = 0;
        void* view = platform::shmCreate(name, sizeof(ShmControlBlock), false, &mapped_size);
        ASSERT_NE(view, static_cast<void*>(NULL));
        EXPECT_EQ(mapped_size, object_size);
        platform::shmDetach(view, mapped_size);
    }
    platform::shmDetach(owner, created_size);
    platform::shmUnlink(name);
}
#endif

TEST_F(ShmTransportTest, RecvReturnsZeroWhenNoData) {
    std::string shm_name = generateShmName("nodata_test");
    ShmTransport server(shm_name, true);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false);
    client.connect("", 0);

    uint8_t recv_buf[64];
    uint32_t from_id = 0;
    EXPECT_EQ(server.serverRecv(recv_buf, sizeof(recv_buf), from_id), 0);
    EXPECT_EQ(client.recv(recv_buf, sizeof(recv_buf)), 0);

    client.close();
    server.close();
}

#ifndef _WIN32
TEST_F(ShmTransportTest, RecvDoesNotConsumeResponseEventFd) {
    std::string shm_name = generateShmName("recv_eventfd_owner_test");
    ShmTransport server(shm_name, true);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false);
    ASSERT_EQ(client.connect("", 0), 0);

    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) {
        platform::sleepMs(1);
    }
    ASSERT_EQ(server.clientCount(), 1u);

    const uint8_t response[] = {1, 2, 3, 4};
    uint32_t client_id = server.activeClientIds()[0];
    ASSERT_EQ(server.serverSend(client_id, response, sizeof(response)),
              static_cast<int>(sizeof(response)));
    ASSERT_TRUE(platform::waitFdReadable(client.fd(), 100));

    uint8_t received[sizeof(response)] = {0};
    ASSERT_EQ(client.recv(received, sizeof(received)),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(memcmp(received, response, sizeof(response)), 0);
    EXPECT_TRUE(platform::waitFdReadable(client.fd(), 0));
    EXPECT_TRUE(platform::eventFdConsume(client.fd()));
    EXPECT_FALSE(platform::waitFdReadable(client.fd(), 0));
}

TEST_F(ShmTransportTest, ServerRecvDoesNotConsumeRequestEventFd) {
    std::string shm_name = generateShmName("server_recv_eventfd_owner_test");
    ShmTransport server(shm_name, true);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false);
    ASSERT_EQ(client.connect("", 0), 0);

    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) {
        platform::sleepMs(1);
    }
    ASSERT_EQ(server.clientCount(), 1u);
    ASSERT_TRUE(platform::waitFdReadable(server.reqEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.reqEventFd()));

    const uint8_t request[] = {5, 6, 7, 8};
    ASSERT_EQ(client.send(request, sizeof(request)),
              static_cast<int>(sizeof(request)));
    ASSERT_TRUE(platform::waitFdReadable(server.reqEventFd(), 100));

    uint8_t received[sizeof(request)] = {0};
    uint32_t from_id = 0;
    ASSERT_EQ(server.serverRecv(received, sizeof(received), from_id),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(memcmp(received, request, sizeof(request)), 0);
    EXPECT_TRUE(platform::waitFdReadable(server.reqEventFd(), 0));
    EXPECT_TRUE(platform::eventFdConsume(server.reqEventFd()));
    EXPECT_FALSE(platform::waitFdReadable(server.reqEventFd(), 0));
}

TEST_F(ShmTransportTest, NonemptyRingDoesNotGeneratePerFrameNotifications) {
    std::string shm_name = generateShmName("transition_notify_test");
    ShmTransport server(shm_name, true);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false);
    ASSERT_EQ(client.connect("", 0), 0);

    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) {
        platform::sleepMs(1);
    }
    ASSERT_EQ(server.clientCount(), 1u);
    ASSERT_TRUE(platform::waitFdReadable(server.reqEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.reqEventFd()));

    const uint32_t first = 1;
    const uint32_t second = 2;
    ASSERT_EQ(client.send(reinterpret_cast<const uint8_t*>(&first), sizeof(first)),
              static_cast<int>(sizeof(first)));
    ASSERT_EQ(client.send(reinterpret_cast<const uint8_t*>(&second), sizeof(second)),
              static_cast<int>(sizeof(second)));
    ASSERT_TRUE(platform::waitFdReadable(server.reqEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.reqEventFd()));
    EXPECT_FALSE(platform::waitFdReadable(server.reqEventFd(), 0));

    uint32_t received = 0;
    uint32_t from_id = 0;
    ASSERT_EQ(server.serverRecv(reinterpret_cast<uint8_t*>(&received), sizeof(received), from_id),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(received, first);
    ASSERT_EQ(server.serverRecv(reinterpret_cast<uint8_t*>(&received), sizeof(received), from_id),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(received, second);
    EXPECT_EQ(server.serverRecv(reinterpret_cast<uint8_t*>(&received), sizeof(received), from_id), 0);
}

TEST_F(ShmTransportTest, ConcurrentEnqueueAndRaceSafeDrainDoesNotStrandFrames) {
    const uint32_t frame_count = 20000;
    std::string shm_name = generateShmName("eventfd_drain_race_test");
    ShmTransport server(shm_name, true, 4096, 4096);
    HandshakeHandlerThread handshake_handler(server);
    ShmTransport client(shm_name, false, 4096, 4096);
    ASSERT_EQ(client.connect("", 0), 0);

    for (int i = 0; i < 50 && server.clientCount() == 0; ++i) {
        platform::sleepMs(1);
    }
    ASSERT_EQ(server.clientCount(), 1u);
    ASSERT_TRUE(platform::waitFdReadable(server.reqEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.reqEventFd()));

    std::atomic<bool> producer_done(false);
    std::atomic<bool> producer_failed(false);
    std::atomic<bool> stop_producer(false);
    std::thread producer([&]() {
        for (uint32_t sequence = 0; sequence < frame_count; ++sequence) {
            int ret = 0;
            while ((ret = client.send(reinterpret_cast<const uint8_t*>(&sequence),
                                      sizeof(sequence))) == 0) {
                if (stop_producer.load()) break;
                std::this_thread::yield();
            }
            if (stop_producer.load()) break;
            if (ret != static_cast<int>(sizeof(sequence))) {
                producer_failed.store(true);
                break;
            }
        }
        producer_done.store(true);
    });

    uint32_t received_count = 0;
    bool data_mismatch = false;
    bool notification_timeout = false;
    bool consume_failed = false;
    while (received_count < frame_count) {
        if (!platform::waitFdReadable(server.reqEventFd(), 1000)) {
            notification_timeout = true;
            stop_producer.store(true);
            break;
        }
        if (!platform::eventFdConsume(server.reqEventFd())) {
            consume_failed = true;
            stop_producer.store(true);
            break;
        }

        while (true) {
            uint32_t sequence = 0;
            uint32_t from_id = 0;
            int ret = server.serverRecv(
                reinterpret_cast<uint8_t*>(&sequence), sizeof(sequence), from_id);
            if (ret == 0) break;
            if (ret != static_cast<int>(sizeof(sequence))
                || sequence != received_count) {
                data_mismatch = true;
            }
            ++received_count;
        }

        if (producer_done.load() && producer_failed.load()) break;
    }

    producer.join();
    EXPECT_FALSE(producer_failed.load());
    EXPECT_FALSE(notification_timeout);
    EXPECT_FALSE(consume_failed);
    EXPECT_FALSE(data_mismatch);
    EXPECT_EQ(received_count, frame_count);
}
#endif

TEST_F(ShmTransportTest, ConcurrentMultiClientSendRecv) {
    const int NUM_CLIENTS = 4;
    const size_t data_size = 4096;
    std::string shm_name = generateShmName("concurrent_test");

    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    HandshakeHandlerThread handshake_handler(server);

    // Create and connect all clients
    std::vector<std::unique_ptr<ShmTransport>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto c = std::unique_ptr<ShmTransport>(new ShmTransport(shm_name, false, 128 * 1024, 128 * 1024));
        ASSERT_EQ(c->connect("", 0), 0);
        clients.push_back(std::move(c));
    }
    // Wait for HandshakeHandlerThread to process all handshakes.
    for (int i = 0; i < 100 && server.clientCount() < static_cast<uint32_t>(NUM_CLIENTS); ++i) {
        platform::sleepMs(1);
    }
    EXPECT_EQ(server.clientCount(), static_cast<uint32_t>(NUM_CLIENTS));

    // All clients send simultaneously
    std::vector<std::vector<uint8_t>> send_data(NUM_CLIENTS);
    std::vector<std::thread> threads;
    std::atomic<int> send_ok{0};

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        send_data[i].resize(data_size);
        for (size_t j = 0; j < data_size; ++j) {
            send_data[i][j] = static_cast<uint8_t>((i * 256 + j) & 0xFF);
        }
        threads.emplace_back([&, i]() {
            ASSERT_EQ(clients[i]->send(send_data[i].data(), data_size),
                      static_cast<int>(data_size));
            send_ok++;
        });
    }

    for (auto& t : threads) t.join();
    EXPECT_EQ(send_ok.load(), NUM_CLIENTS);

    // Server receives all
    platform::sleepMs(50);
    int received = 0;
    for (int i = 0; i < NUM_CLIENTS * 10 && received < NUM_CLIENTS; ++i) {
        std::vector<uint8_t> buf(data_size);
        uint32_t from_id = 0;
        int ret = server.serverRecv(buf.data(), data_size, from_id);
        if (ret > 0) {
            EXPECT_GE(from_id, 1u);
            received++;
        }
        platform::sleepMs(5);
    }
    EXPECT_EQ(received, NUM_CLIENTS);

    for (auto& c : clients) c->close();
    server.close();
}

TEST_F(ShmTransportTest, ConcurrentDataIntegrity) {
    const int NUM_CLIENTS = 3;
    const int ROUNDS = 50;
    std::string shm_name = generateShmName("integrity_test");

    ShmTransport server(shm_name, true, 128 * 1024, 128 * 1024);
    HandshakeHandlerThread handshake_handler(server);

    // Create clients
    std::vector<std::unique_ptr<ShmTransport>> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        auto c = std::unique_ptr<ShmTransport>(new ShmTransport(shm_name, false, 128 * 1024, 128 * 1024));
        ASSERT_EQ(c->connect("", 0), 0);
        clients.push_back(std::move(c));
    }

    // Multi-round: each client sends, server echoes back
    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            uint32_t marker = static_cast<uint32_t>((round << 16) | (i << 8) | 0xAA);
            std::vector<uint8_t> send_buf(sizeof(marker));
            memcpy(send_buf.data(), &marker, sizeof(marker));

            ASSERT_EQ(clients[i]->send(send_buf.data(), sizeof(marker)),
                      static_cast<int>(sizeof(marker)));
        }

        platform::sleepMs(10);

        // Server recv all and echo back
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            std::vector<uint8_t> recv_buf(64);
            uint32_t from_id = 0;
            int ret = server.serverRecv(recv_buf.data(), recv_buf.size(), from_id);
            ASSERT_GT(ret, 0);
            // Echo back
            ASSERT_EQ(server.serverSend(from_id, recv_buf.data(), static_cast<size_t>(ret)),
                      ret);
        }

        // Clients verify echo
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            std::vector<uint8_t> resp(64);
            int ret = clients[i]->recv(resp.data(), resp.size());
            ASSERT_GT(ret, 0);
            uint32_t echoed = 0;
            memcpy(&echoed, resp.data(), sizeof(echoed));
            uint32_t expected = static_cast<uint32_t>((round << 16) | (i << 8) | 0xAA);
            EXPECT_EQ(echoed, expected) << "round=" << round << " client=" << i;
        }
    }

    for (auto& c : clients) c->close();
    server.close();
}
