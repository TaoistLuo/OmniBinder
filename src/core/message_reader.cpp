#include "core/message_reader.h"

namespace omnibinder {

int readNextMessage(IMessageConnection& transport, Buffer& recv_buffer, Message& out)
{
    transport.consumeReadiness();

    // ---- 成帧传输（SHM）：recv 返回一条完整序列化 Message ----
    if (transport.isFramed()) {
        size_t frame_size = 0;
        int ready = transport.peekFrameSize(frame_size);
        if (ready <= 0) {
            return ready;
        }
        if (frame_size < MESSAGE_HEADER_SIZE || frame_size > MAX_MESSAGE_SIZE) {
            return -1;
        }

        // 复用持久 recv_buffer 作接收暂存，避免每条消息一次堆分配
        recv_buffer.resize(frame_size);
        if (recv_buffer.capacity() < frame_size) {
            return -1;
        }
        int ret = transport.recv(recv_buffer.mutableData(), frame_size);
        if (ret <= 0) {
            return ret;
        }
        if (static_cast<size_t>(ret) != frame_size) {
            return -1;
        }

        MessageHeader header;
        if (!Message::parseHeader(recv_buffer.data(), frame_size, header)
            || !Message::validateHeader(header)
            || header.length != frame_size - MESSAGE_HEADER_SIZE) {
            return -1;
        }

        out.header = header;
        out.payload.clear();
        if (header.length > 0) {
            out.payload.assign(recv_buffer.data() + MESSAGE_HEADER_SIZE, header.length);
        }
        return 1;
    }

    // ---- 字节流传输（TCP）：先抽取已缓冲的完整帧，不足再读一块 ----
    while (true) {
        size_t pos = recv_buffer.readPosition();
        size_t avail = recv_buffer.size() - pos;

        MessageHeader header;
        size_t frame_size = 0;
        FrameStatus status = (avail == 0)
            ? FrameStatus::NeedMore
            : tryExtractFrame(recv_buffer.data() + pos, avail, header, frame_size);

        if (status == FrameStatus::Complete) {
            out.header = header;
            out.payload.clear();
            if (header.length > 0) {
                out.payload.assign(recv_buffer.data() + pos + MESSAGE_HEADER_SIZE,
                                   header.length);
            }
            if (!recv_buffer.trySetReadPosition(pos + frame_size)) {
                return -1;
            }
            recv_buffer.compact();
            return 1;
        }
        if (status == FrameStatus::Corrupt) {
            return -1;
        }

        // 直接读入 recv_buffer 尾部，省去"栈 chunk 中转"的一次拷贝
        size_t space = 0;
        uint8_t* tail = recv_buffer.writableTail(space);
        if (!tail) {
            return -1;
        }
        int ret = transport.recv(tail, space);
        if (ret <= 0) {
            return ret;
        }
        if (recv_buffer.remaining() + static_cast<size_t>(ret) > MAX_MESSAGE_SIZE) {
            return -1;
        }
        if (!recv_buffer.commitWritten(static_cast<size_t>(ret))) {
            return -1;
        }
    }
}

} // namespace omnibinder
