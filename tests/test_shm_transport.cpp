#include <gtest/gtest.h>
#include "transport/shm_client_transport.h"
#include "transport/shm_server_transport.h"
#include "platform/platform.h"
#include "core/event_loop.h"
#include "core/topic_runtime.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <sstream>

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

/* @brief RAII 辅助：后台线程在服务端端点上接受 handshake 连接并完成每次不透明句柄交换
 * @details 客户端 connect() 会阻塞，直到服务端接受连接并发送通知句柄。 */
class HandshakeAcceptor {
public:
    explicit HandshakeAcceptor(ShmServerTransport& server)
        : server_(server), stop_(false)
    {
        server_.setAcceptCallback([this](int client_id, IClientTransport* client) {
            std::lock_guard<std::mutex> lock(mutex_);
            clients_[client_id] = client;
        });
        thread_ = std::thread([this]() {
            int listen_fd = server_.handshakeListenFd();
            while (!stop_.load()) {
                if (platform::waitFdReadable(listen_fd, 10)) {
                    server_.onPollEvent(listen_fd, EventLoop::EVENT_READ);
                }
            }
        });
    }

    ~HandshakeAcceptor() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    size_t clientCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.size();
    }

    bool waitForClientCount(size_t expected, uint32_t timeout_ms = 2000) {
        for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
            if (clientCount() == expected) return true;
            platform::sleepMs(1);
        }
        return clientCount() == expected;
    }

    IClientTransport* firstClient(uint32_t timeout_ms = 2000) {
        for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!clients_.empty()) return clients_.begin()->second;
            platform::sleepMs(1);
        }
        return NULL;
    }

    std::vector<std::pair<int, IClientTransport*> > clients() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<int, IClientTransport*> > result;
        for (std::map<int, IClientTransport*>::iterator it = clients_.begin();
             it != clients_.end(); ++it) {
            result.push_back(std::make_pair(it->first, it->second));
        }
        return result;
    }

private:
    ShmServerTransport& server_;
    std::atomic<bool> stop_;
    std::thread thread_;
    std::mutex mutex_;
    std::map<int, IClientTransport*> clients_;
};

/* @brief RAII 辅助：在专用 owner EventLoop 上驱动服务端端点
 * @details 与 core 注册 pollFds() 并分派 onPollEvent() 的方式一致。 */
class ShmServerLoop {
public:
    explicit ShmServerLoop(ShmServerTransport& server)
        : server_(server), ready_(false)
        , accepted_count_(0), cleanup_count_(0), timer_count_(0)
        , readable_frames_(0), wrong_thread_(false)
    {
        server_.setAcceptCallback([this](int client_id, IClientTransport* client) {
            std::lock_guard<std::mutex> lock(mutex_);
            clients_[client_id] = client;
            accepted_count_++;
        });
        server_.setReadableCallback([this](int client_id) {
            IClientTransport* client = NULL;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::map<int, IClientTransport*>::iterator it = clients_.find(client_id);
                if (it != clients_.end()) client = it->second;
            }
            if (!client) return;
            while (true) {
                size_t frame_size = 0;
                int ready = client->peekFrameSize(frame_size);
                if (ready <= 0) break;
                std::vector<uint8_t> frame(frame_size);
                int ret = client->recv(frame.data(), frame.size());
                if (ret <= 0) break;
                std::lock_guard<std::mutex> lock(mutex_);
                received_.insert(received_.end(), frame.begin(), frame.begin() + ret);
                readable_frames_++;
            }
        });
        server_.setDisconnectCallback([this](int client_id) {
            if (std::this_thread::get_id() != owner_thread_) {
                wrong_thread_.store(true);
            }
            int fd = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::map<int, IClientTransport*>::iterator it = clients_.find(client_id);
                if (it != clients_.end()) {
                    fd = it->second->fd();
                    clients_.erase(it);
                }
            }
            if (fd >= 0) {
                loop_.removeFd(fd);
                registered_.erase(fd);
            }
            server_.removeClient(client_id);
            cleanup_count_++;
        });

        thread_ = std::thread([this]() {
            owner_thread_ = std::this_thread::get_id();
            syncFds();
            loop_.addTimer(20, [this]() { ++timer_count_; }, true);
            ready_.store(true);
            loop_.run();
        });
        while (!ready_.load()) std::this_thread::yield();
    }

    ~ShmServerLoop() {
        loop_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void syncFds() {
        std::vector<int> fds;
        server_.pollFds(fds);
        std::set<int> wanted(fds.begin(), fds.end());

        for (std::set<int>::iterator it = registered_.begin(); it != registered_.end();) {
            if (wanted.find(*it) == wanted.end()) {
                loop_.removeFd(*it);
                registered_.erase(it++);
            } else {
                ++it;
            }
        }
        for (size_t i = 0; i < fds.size(); ++i) {
            if (registered_.find(fds[i]) != registered_.end()) continue;
            registered_.insert(fds[i]);
            loop_.addFd(fds[i], EventLoop::EVENT_READ | EventLoop::EVENT_ERROR,
                [this](int fd, uint32_t ev) {
                    server_.onPollEvent(fd, ev);
                    syncFds();
                });
        }
    }

    void notifyMaster() {
        platform::eventFdNotify(server_.requestEventFd());
    }

    size_t clientCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.size();
    }

    bool waitForClientCount(size_t expected, uint32_t timeout_ms = 1000) {
        for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
            if (clientCount() == expected) return true;
            platform::sleepMs(1);
        }
        return clientCount() == expected;
    }

    bool waitForCleanupCount(uint32_t expected, uint32_t timeout_ms = 1000) {
        for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
            if (cleanup_count_.load() == expected) return true;
            platform::sleepMs(1);
        }
        return cleanup_count_.load() == expected;
    }

    uint32_t acceptedCount() const { return accepted_count_.load(); }
    uint32_t cleanupCount() const { return cleanup_count_.load(); }
    uint32_t timerCount() const { return timer_count_.load(); }
    bool cleanupRanOnOwnerThread() const { return !wrong_thread_.load(); }

private:
    ShmServerTransport& server_;
    EventLoop loop_;
    std::thread thread_;
    std::thread::id owner_thread_;
    std::atomic<bool> ready_;
    std::atomic<uint32_t> accepted_count_;
    std::atomic<uint32_t> cleanup_count_;
    std::atomic<uint32_t> timer_count_;
    std::atomic<uint32_t> readable_frames_;
    std::atomic<bool> wrong_thread_;
    std::mutex mutex_;
    std::map<int, IClientTransport*> clients_;
    std::vector<uint8_t> received_;
    std::set<int> registered_;
};

class ShmTransportTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { platform::netInit(); }
    static void TearDownTestSuite() { platform::netCleanup(); }
};

static bool waitForFrame(IClientTransport* client, uint32_t timeout_ms = 2000)
{
    for (uint32_t elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        size_t frame_size = 0;
        if (client->peekFrameSize(frame_size) > 0) return true;
        platform::sleepMs(1);
    }
    return false;
}

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

// ============================================================
// shm_ring 纯函数
// ============================================================

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

// ============================================================
// 服务端 / 客户端生命周期
// ============================================================

TEST_F(ShmTransportTest, ServerCreate) {
    ShmServerTransport server("srv_create_test");
    EXPECT_EQ(server.type(), TransportType::SHM);
    EXPECT_EQ(server.clientCount(), 0u);

    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    EXPECT_GE(server.handshakeListenFd(), 0);
    EXPECT_GE(server.requestEventFd(), 0);
    server.close();
    EXPECT_EQ(server.handshakeListenFd(), -1);
}

TEST_F(ShmTransportTest, ClientConnect) {
    ShmServerTransport server("client_conn_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);

    ShmClientTransport client("client_conn_test");
    EXPECT_EQ(client.state(), ConnectionState::DISCONNECTED);
    EXPECT_EQ(client.type(), TransportType::SHM);

    ASSERT_EQ(client.connect("", 0), 0);
    EXPECT_EQ(client.state(), ConnectionState::CONNECTED);
    EXPECT_GE(client.fd(), 0);
    EXPECT_TRUE(acceptor.waitForClientCount(1));

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, MultipleClientsConnect) {
    ShmServerTransport server("multi_client_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);

    ShmClientTransport client0("multi_client_test");
    ShmClientTransport client1("multi_client_test");
    ShmClientTransport client2("multi_client_test");
    ASSERT_EQ(client0.connect("", 0), 0);
    ASSERT_EQ(client1.connect("", 0), 0);
    ASSERT_EQ(client2.connect("", 0), 0);

    EXPECT_TRUE(acceptor.waitForClientCount(3));

    client0.close();
    client1.close();
    client2.close();
    server.close();
}

TEST_F(ShmTransportTest, CleanCloseReclaimsExactlyOnceOnOwnerLoop) {
    ShmServerTransport server("clean_liveness_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    ShmServerLoop owner(server);
    ShmClientTransport client("clean_liveness_test");

    ASSERT_EQ(client.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));
    ASSERT_EQ(owner.acceptedCount(), 1u);

    client.close();
    ASSERT_TRUE(owner.waitForCleanupCount(1));
    ASSERT_TRUE(owner.waitForClientCount(0));
    EXPECT_EQ(owner.cleanupCount(), 1u);
    EXPECT_TRUE(owner.cleanupRanOnOwnerThread());
    platform::sleepMs(10);
    EXPECT_EQ(owner.cleanupCount(), 1u);
}

TEST_F(ShmTransportTest, ClosedClientDoesNotHarmSurvivingClient) {
    ShmServerTransport server("surviving_client_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport departing("surviving_client_test");
    ShmClientTransport survivor("surviving_client_test");

    ASSERT_EQ(departing.connect("", 0), 0);
    ASSERT_EQ(survivor.connect("", 0), 0);
    ASSERT_TRUE(acceptor.waitForClientCount(2));

    std::vector<std::pair<int, IClientTransport*> > conns = acceptor.clients();
    ASSERT_EQ(conns.size(), 2u);

    departing.close();
    platform::sleepMs(10);

    const uint8_t response[] = {9, 8, 7};
    for (size_t i = 0; i < conns.size(); ++i) {
        ASSERT_EQ(conns[i].second->send(response, sizeof(response)),
                  static_cast<int>(sizeof(response)));
    }

    ASSERT_TRUE(platform::waitFdReadable(survivor.fd(), 200));
    uint8_t received[sizeof(response)] = {0};
    EXPECT_EQ(survivor.recv(received, sizeof(received)), static_cast<int>(sizeof(received)));
    EXPECT_EQ(memcmp(received, response, sizeof(response)), 0);
}

#ifndef _WIN32
TEST_F(ShmTransportTest, AbruptPeerDeathReclaimsAndAllowsReconnect) {
    ShmServerTransport server("abrupt_liveness_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    ShmServerLoop owner(server);

    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ShmClientTransport client("abrupt_liveness_test");
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

    ShmClientTransport reconnected("abrupt_liveness_test");
    ASSERT_EQ(reconnected.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));
    reconnected.close();
    ASSERT_TRUE(owner.waitForClientCount(0));
    ASSERT_TRUE(owner.waitForCleanupCount(2));
    EXPECT_EQ(owner.cleanupCount(), 2u);
    EXPECT_TRUE(owner.cleanupRanOnOwnerThread());
}

TEST_F(ShmTransportTest, StalledHandshakeOnlyBoundsOwnerLoopOnce) {
    ShmServerTransport server("stalled_owner_loop_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    ShmServerLoop owner(server);
    uint32_t timer_before = owner.timerCount();
    int peer_fd = connectRawHandshakePeer(server.handshakeListenFd());
    ASSERT_GE(peer_fd, 0);

    platform::sleepMs(750);
    EXPECT_GT(owner.timerCount(), timer_before);
    EXPECT_EQ(server.clientCount(), 0u);

    ShmClientTransport healthy_client("stalled_owner_loop_test");
    ASSERT_EQ(healthy_client.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));
    healthy_client.close();
    ASSERT_TRUE(owner.waitForClientCount(0));
    close(peer_fd);
}

TEST_F(ShmTransportTest, UnexpectedReadableHandshakeDataUsesLivenessCleanup) {
    std::string server_name = "readable_liveness_test";
    ShmServerTransport server(server_name);
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    ShmServerLoop owner(server);

    std::ostringstream client_name_builder;
    client_name_builder << "omni_readable_client_" << getpid();
    std::string client_name = client_name_builder.str();
    const uint32_t capacity = static_cast<uint32_t>(SHM_DEFAULT_REQ_RING_CAPACITY);
    size_t shm_size = calculateShmSize(capacity, capacity);
    void* addr = platform::shmCreate(client_name, shm_size, true);
    ASSERT_NE(addr, static_cast<void*>(NULL));
    shmInitLayout(addr, shm_size, capacity, capacity);

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

// ============================================================
// 数据面读写
// ============================================================

TEST_F(ShmTransportTest, ClientSendServerRecv) {
    const size_t data_size = 64 * 1024;
    const size_t ring_size = 128 * 1024;
    ShmServerTransport server("c2s_test", ring_size, ring_size);
    ASSERT_EQ(server.start("", 0, TransportConfig(ring_size, ring_size)), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("c2s_test", ring_size, ring_size);
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));

    std::vector<uint8_t> send_data(data_size);
    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    ASSERT_EQ(client.send(send_data.data(), data_size), static_cast<int>(data_size));
    ASSERT_TRUE(waitForFrame(conn));

    size_t frame_size = 0;
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(frame_size, data_size);
    std::vector<uint8_t> recv_data(data_size);
    ASSERT_EQ(conn->recv(recv_data.data(), recv_data.size()), static_cast<int>(data_size));
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    for (size_t i = 0; i < data_size; i++) {
        send_data[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }
    ASSERT_EQ(conn->send(send_data.data(), data_size), static_cast<int>(data_size));

    ASSERT_TRUE(platform::waitFdReadable(client.fd(), 200));
    memset(recv_data.data(), 0, data_size);
    ASSERT_EQ(client.recv(recv_data.data(), data_size), static_cast<int>(data_size));
    EXPECT_EQ(memcmp(send_data.data(), recv_data.data(), data_size), 0);

    client.close();
    server.close();
}

TEST_F(ShmTransportTest, Delivers65537ByteFramesBothDirections) {
    const size_t data_size = 65537;
    const size_t ring_size = 128 * 1024;
    ShmServerTransport server("frame_65537_test", ring_size, ring_size);
    ASSERT_EQ(server.start("", 0, TransportConfig(ring_size, ring_size)), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("frame_65537_test", ring_size, ring_size);
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));

    std::vector<uint8_t> request(data_size);
    for (size_t i = 0; i < request.size(); ++i) request[i] = static_cast<uint8_t>(i * 31u + 7u);
    ASSERT_EQ(client.send(request.data(), request.size()), static_cast<int>(request.size()));

    ASSERT_TRUE(waitForFrame(conn));
    size_t frame_size = 0;
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(frame_size, request.size());
    std::vector<uint8_t> received(frame_size);
    ASSERT_EQ(conn->recv(received.data(), received.size()), static_cast<int>(request.size()));
    EXPECT_EQ(received, request);

    std::vector<uint8_t> response(data_size);
    for (size_t i = 0; i < response.size(); ++i) response[i] = static_cast<uint8_t>(i * 17u + 3u);
    ASSERT_EQ(conn->send(response.data(), response.size()),
              static_cast<int>(response.size()));
    ASSERT_EQ(client.peekFrameSize(frame_size), 1);
    ASSERT_EQ(frame_size, response.size());
    received.resize(frame_size);
    ASSERT_EQ(client.recv(received.data(), received.size()), static_cast<int>(response.size()));
    EXPECT_EQ(received, response);
}

TEST_F(ShmTransportTest, LargerConfiguredRingPreservesPayloadAndLargeThenSmallOrder) {
    const size_t large_size = 100 * 1024;
    const size_t ring_size = 256 * 1024;
    ShmServerTransport server("large_then_small_test", ring_size, ring_size);
    ASSERT_EQ(server.start("", 0, TransportConfig(ring_size, ring_size)), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("large_then_small_test", ring_size, ring_size);
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));

    std::vector<uint8_t> large(large_size);
    for (size_t i = 0; i < large.size(); ++i) large[i] = static_cast<uint8_t>((i ^ (i >> 8)) & 0xffu);
    const uint8_t small[] = {0xde, 0xad, 0xbe, 0xef};
    ASSERT_EQ(client.send(large.data(), large.size()), static_cast<int>(large.size()));
    ASSERT_EQ(client.send(small, sizeof(small)), static_cast<int>(sizeof(small)));

    ASSERT_TRUE(waitForFrame(conn));
    size_t frame_size = 0;
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(frame_size, large.size());
    std::vector<uint8_t> received(frame_size);
    ASSERT_EQ(conn->recv(received.data(), received.size()),
              static_cast<int>(large.size()));
    EXPECT_EQ(received, large);
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(frame_size, sizeof(small));
    received.resize(frame_size);
    ASSERT_EQ(conn->recv(received.data(), received.size()),
              static_cast<int>(sizeof(small)));
    EXPECT_EQ(memcmp(received.data(), small, sizeof(small)), 0);
}

TEST_F(ShmTransportTest, RecvReturnsZeroWhenNoData) {
    ShmServerTransport server("nodata_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("nodata_test");
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));

    uint8_t recv_buf[64];
    size_t frame_size = 0;
    EXPECT_EQ(conn->peekFrameSize(frame_size), 0);
    EXPECT_EQ(conn->recv(recv_buf, sizeof(recv_buf)), 0);
    EXPECT_EQ(client.recv(recv_buf, sizeof(recv_buf)), 0);

    client.close();
    server.close();
}

#ifndef _WIN32
TEST_F(ShmTransportTest, ImpossibleHeadDeterministicallyDisconnectsClient) {
    const uint32_t ring_size = 128 * 1024;
    ShmServerTransport server("invalid_head_disconnect_test", ring_size, ring_size);
    ASSERT_EQ(server.start("", 0, TransportConfig(ring_size, ring_size)), 0);
    ShmServerLoop owner(server);
    ShmClientTransport client("invalid_head_disconnect_test", ring_size, ring_size);
    ASSERT_EQ(client.connect("", 0), 0);
    ASSERT_TRUE(owner.waitForClientCount(1));

    size_t mapped_size = 0;
    void* addr = platform::shmCreate(client.shmName(), 1, false, &mapped_size);
    ASSERT_NE(addr, static_cast<void*>(NULL));
    ASSERT_GE(mapped_size, calculateShmSize(ring_size, ring_size));
    ShmRingHeader* req_ring = shmRequestRingFromBase(static_cast<uint8_t*>(addr));
    uint8_t* req_data = shmRequestDataFromBase(static_cast<uint8_t*>(addr));
    uint32_t impossible_length = ring_size;
    memcpy(req_data, &impossible_length, sizeof(impossible_length));
    req_ring->read_pos.store(0, std::memory_order_release);
    req_ring->write_pos.store(sizeof(impossible_length), std::memory_order_release);

    owner.notifyMaster();
    EXPECT_TRUE(owner.waitForCleanupCount(1));
    EXPECT_TRUE(owner.waitForClientCount(0));
    platform::sleepMs(20);
    EXPECT_EQ(owner.cleanupCount(), 1u);
    platform::shmDetach(addr, mapped_size);
}

TEST_F(ShmTransportTest, RecvDoesNotConsumeResponseEventFd) {
    ShmServerTransport server("recv_eventfd_owner_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("recv_eventfd_owner_test");
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));

    const uint8_t response[] = {1, 2, 3, 4};
    ASSERT_EQ(conn->send(response, sizeof(response)),
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
    ShmServerTransport server("server_recv_eventfd_owner_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("server_recv_eventfd_owner_test");
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));
    ASSERT_TRUE(platform::waitFdReadable(server.requestEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.requestEventFd()));

    const uint8_t request[] = {5, 6, 7, 8};
    ASSERT_EQ(client.send(request, sizeof(request)),
              static_cast<int>(sizeof(request)));
    ASSERT_TRUE(platform::waitFdReadable(server.requestEventFd(), 100));

    uint8_t received[sizeof(request)] = {0};
    size_t frame_size = 0;
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(conn->recv(received, sizeof(received)),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(memcmp(received, request, sizeof(request)), 0);
    EXPECT_TRUE(platform::waitFdReadable(server.requestEventFd(), 0));
    EXPECT_TRUE(platform::eventFdConsume(server.requestEventFd()));
    EXPECT_FALSE(platform::waitFdReadable(server.requestEventFd(), 0));
}

TEST_F(ShmTransportTest, NonemptyRingDoesNotGeneratePerFrameNotifications) {
    ShmServerTransport server("transition_notify_test");
    ASSERT_EQ(server.start("", 0, TransportConfig()), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("transition_notify_test");
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));
    ASSERT_TRUE(platform::waitFdReadable(server.requestEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.requestEventFd()));

    const uint32_t first = 1;
    const uint32_t second = 2;
    ASSERT_EQ(client.send(reinterpret_cast<const uint8_t*>(&first), sizeof(first)),
              static_cast<int>(sizeof(first)));
    ASSERT_EQ(client.send(reinterpret_cast<const uint8_t*>(&second), sizeof(second)),
              static_cast<int>(sizeof(second)));
    ASSERT_TRUE(platform::waitFdReadable(server.requestEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.requestEventFd()));
    EXPECT_FALSE(platform::waitFdReadable(server.requestEventFd(), 0));

    uint32_t received = 0;
    size_t frame_size = 0;
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(conn->recv(reinterpret_cast<uint8_t*>(&received), sizeof(received)),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(received, first);
    ASSERT_EQ(conn->peekFrameSize(frame_size), 1);
    ASSERT_EQ(conn->recv(reinterpret_cast<uint8_t*>(&received), sizeof(received)),
              static_cast<int>(sizeof(received)));
    EXPECT_EQ(received, second);
    EXPECT_EQ(conn->peekFrameSize(frame_size), 0);
}

TEST_F(ShmTransportTest, ConcurrentEnqueueAndRaceSafeDrainDoesNotStrandFrames) {
    const uint32_t frame_count = 20000;
    ShmServerTransport server("eventfd_drain_race_test", 4096, 4096);
    ASSERT_EQ(server.start("", 0, TransportConfig(4096, 4096)), 0);
    HandshakeAcceptor acceptor(server);
    ShmClientTransport client("eventfd_drain_race_test", 4096, 4096);
    ASSERT_EQ(client.connect("", 0), 0);
    IClientTransport* conn = acceptor.firstClient();
    ASSERT_NE(conn, static_cast<IClientTransport*>(NULL));
    ASSERT_TRUE(platform::waitFdReadable(server.requestEventFd(), 100));
    ASSERT_TRUE(platform::eventFdConsume(server.requestEventFd()));

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
        if (!platform::waitFdReadable(server.requestEventFd(), 1000)) {
            notification_timeout = true;
            stop_producer.store(true);
            break;
        }
        if (!platform::eventFdConsume(server.requestEventFd())) {
            consume_failed = true;
            stop_producer.store(true);
            break;
        }

        while (true) {
            uint32_t sequence = 0;
            size_t frame_size = 0;
            int ready = conn->peekFrameSize(frame_size);
            if (ready <= 0) break;
            int ret = conn->recv(reinterpret_cast<uint8_t*>(&sequence), sizeof(sequence));
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
    ShmServerTransport server("concurrent_test", 128 * 1024, 128 * 1024);
    ASSERT_EQ(server.start("", 0, TransportConfig(128 * 1024, 128 * 1024)), 0);
    HandshakeAcceptor acceptor(server);

    std::vector<std::unique_ptr<ShmClientTransport> > clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        std::unique_ptr<ShmClientTransport> c(
            new ShmClientTransport("concurrent_test", 128 * 1024, 128 * 1024));
        ASSERT_EQ(c->connect("", 0), 0);
        clients.push_back(std::move(c));
    }
    ASSERT_TRUE(acceptor.waitForClientCount(static_cast<size_t>(NUM_CLIENTS)));

    std::vector<std::vector<uint8_t> > send_data(NUM_CLIENTS);
    std::vector<std::thread> threads;
    std::atomic<int> send_ok(0);
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        send_data[i].resize(data_size);
        for (size_t j = 0; j < data_size; ++j) {
            send_data[i][j] = static_cast<uint8_t>((i * 256 + j) & 0xFF);
        }
        threads.push_back(std::thread([&, i]() {
            if (clients[i]->send(send_data[i].data(), data_size)
                == static_cast<int>(data_size)) {
                send_ok++;
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
    EXPECT_EQ(send_ok.load(), NUM_CLIENTS);

    std::vector<std::pair<int, IClientTransport*> > conns = acceptor.clients();
    ASSERT_EQ(conns.size(), static_cast<size_t>(NUM_CLIENTS));

    std::set<int> done;
    for (int attempt = 0; attempt < 5000 && done.size() < static_cast<size_t>(NUM_CLIENTS); ++attempt) {
        for (size_t c = 0; c < conns.size(); ++c) {
            if (done.find(conns[c].first) != done.end()) continue;
            size_t frame_size = 0;
            if (conns[c].second->peekFrameSize(frame_size) <= 0) continue;
            std::vector<uint8_t> buf(frame_size);
            int ret = conns[c].second->recv(buf.data(), buf.size());
            ASSERT_EQ(ret, static_cast<int>(data_size));
            done.insert(conns[c].first);
        }
        if (done.size() < static_cast<size_t>(NUM_CLIENTS)) platform::sleepMs(1);
    }
    EXPECT_EQ(done.size(), static_cast<size_t>(NUM_CLIENTS));

    for (size_t i = 0; i < clients.size(); ++i) clients[i]->close();
    server.close();
}

TEST_F(ShmTransportTest, ConcurrentDataIntegrity) {
    const int NUM_CLIENTS = 3;
    const int ROUNDS = 50;
    ShmServerTransport server("integrity_test", 128 * 1024, 128 * 1024);
    ASSERT_EQ(server.start("", 0, TransportConfig(128 * 1024, 128 * 1024)), 0);
    HandshakeAcceptor acceptor(server);

    std::vector<std::unique_ptr<ShmClientTransport> > clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        std::unique_ptr<ShmClientTransport> c(
            new ShmClientTransport("integrity_test", 128 * 1024, 128 * 1024));
        ASSERT_EQ(c->connect("", 0), 0);
        clients.push_back(std::move(c));
    }
    ASSERT_TRUE(acceptor.waitForClientCount(static_cast<size_t>(NUM_CLIENTS)));
    std::vector<std::pair<int, IClientTransport*> > conns = acceptor.clients();
    ASSERT_EQ(conns.size(), static_cast<size_t>(NUM_CLIENTS));

    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < NUM_CLIENTS; ++i) {
            uint32_t marker = static_cast<uint32_t>((round << 16) | (i << 8) | 0xAA);
            std::vector<uint8_t> send_buf(sizeof(marker));
            memcpy(send_buf.data(), &marker, sizeof(marker));
            ASSERT_EQ(clients[i]->send(send_buf.data(), sizeof(marker)),
                      static_cast<int>(sizeof(marker)));
        }

        for (size_t c = 0; c < conns.size(); ++c) {
            ASSERT_TRUE(waitForFrame(conns[c].second));
            size_t frame_size = 0;
            ASSERT_EQ(conns[c].second->peekFrameSize(frame_size), 1);
            std::vector<uint8_t> recv_buf(frame_size);
            int ret = conns[c].second->recv(recv_buf.data(), recv_buf.size());
            ASSERT_GT(ret, 0);
            ASSERT_EQ(conns[c].second->send(recv_buf.data(), static_cast<size_t>(ret)), ret);
        }

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

    for (size_t i = 0; i < clients.size(); ++i) clients[i]->close();
    server.close();
}

#ifndef _WIN32
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
