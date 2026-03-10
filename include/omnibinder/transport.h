/**************************************************************************************************
 * @file        transport.h
 * @brief       传输层抽象
 * @details     定义传输层的抽象接口，包括客户端侧 ITransport（connect/send/recv）
 *              和服务端侧 ITransportServer（listen/accept）。所有 I/O 均为非阻塞设计，
 *              配合 EventLoop 使用。具体实现包括 TcpTransport 和 ShmTransport。
 *
 * @author      taoist.luo
 * @version     1.0.0
 * @date        2026-02-11
 *
 * Copyright (c) 2025 taoist.luo (https://github.com/TaoistLuo/OmniBinder)
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *************************************************************************************************/
#ifndef OMNIBINDER_TRANSPORT_H
#define OMNIBINDER_TRANSPORT_H

#include <string>
#include <stdint.h>
#include <stddef.h>

namespace omnibinder {

// ============================================================
// Transport types and connection states
// ============================================================

enum class TransportType {
    TCP,
    SHM,
};

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR,
};

// ============================================================
// Forward declaration
// ============================================================
class ITransport;

// ============================================================
// ITransportServer - Server-side transport interface
//
// Creates a listening endpoint and accepts incoming connections.
// Designed for non-blocking use with EventLoop: register fd()
// for read events, then call accept() when readable.
// ============================================================

class ITransportServer {
public:
    virtual ~ITransportServer() {}

    // Start listening on the given host and port.
    // Pass port=0 to let the OS assign an ephemeral port.
    // Returns the actual port on success, or -1 on failure.
    virtual int listen(const std::string& host, uint16_t port) = 0;

    // Stop listening and release the socket.
    virtual void close() = 0;

    // Returns the port this server is listening on, or 0 if not listening.
    virtual uint16_t port() const = 0;

    // Returns the underlying file descriptor for EventLoop registration.
    // Returns -1 if not listening.
    virtual int fd() const = 0;

    // Accept a pending connection. Returns a new ITransport in CONNECTED
    // state, or NULL if no connection is pending (EAGAIN) or on error.
    // Caller takes ownership of the returned pointer.
    virtual ITransport* accept() = 0;
};

// ============================================================
// ITransport - Client-side transport interface
//
// Represents a single bidirectional byte-stream connection.
// All I/O is non-blocking. Designed for use with EventLoop:
// register fd() for read/write events as needed.
// ============================================================

class ITransport {
public:
    virtual ~ITransport() {}

    // Initiate a non-blocking connection to the given host and port.
    // Returns 0 on immediate success, 1 if connection is in progress
    // (state becomes CONNECTING), or -1 on failure (state becomes ERROR).
    virtual int connect(const std::string& host, uint16_t port) = 0;

    // Send data over the connection. Handles partial writes internally,
    // looping until all bytes are sent or the socket would block.
    // Returns the number of bytes sent (may be less than length if
    // the socket would block), or -1 on error.
    virtual int send(const uint8_t* data, size_t length) = 0;

    // Receive data from the connection (non-blocking).
    // Returns the number of bytes read, 0 if the socket would block
    // (no data available), or -1 on error (including peer disconnect).
    virtual int recv(uint8_t* buf, size_t buf_size) = 0;

    // Close the connection and release the socket.
    virtual void close() = 0;

    // Returns the current connection state.
    virtual ConnectionState state() const = 0;

    // Returns the underlying file descriptor for EventLoop registration.
    // Returns -1 if not connected.
    virtual int fd() const = 0;

    // Returns the transport type.
    virtual TransportType type() const = 0;
};

} // namespace omnibinder

#endif // OMNIBINDER_TRANSPORT_H
