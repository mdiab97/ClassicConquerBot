#pragma once
#include "common/net/message.h"
#include <asio.hpp>
#include <memory>
#include <deque>
#include <functional>
#include <thread>
#include <iostream>

namespace ccbot::net {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using Ptr = std::shared_ptr<TcpConnection>;

    TcpConnection(asio::ip::tcp::socket socket, uint32_t id,
                  std::function<void(Ptr, Message)> on_message,
                  std::function<void(Ptr)> on_disconnect)
        : socket_(std::move(socket))
        , id_(id)
        , on_message_(std::move(on_message))
        , on_disconnect_(std::move(on_disconnect))
    {}

    uint32_t id() const { return id_; }
    bool is_connected() const { return socket_.is_open(); }

    void start() { read_header(); }

    void send(const Message& msg) {
        asio::post(socket_.get_executor(), [this, self = shared_from_this(), msg]() {
            bool writing = !out_queue_.empty();
            out_queue_.push_back(msg);
            if (!writing) write_header();
        });
    }

    void disconnect() {
        if (socket_.is_open()) {
            asio::post(socket_.get_executor(), [this, self = shared_from_this()]() {
                socket_.close();
            });
        }
    }

private:
    void read_header() {
        asio::async_read(socket_,
            asio::buffer(&in_msg_.header, sizeof(MessageHeader)),
            [this, self = shared_from_this()](std::error_code ec, size_t) {
                if (!ec) {
                    if (in_msg_.header.size > 0) {
                        in_msg_.body.resize(in_msg_.header.size);
                        read_body();
                    } else {
                        in_msg_.body.clear();
                        if (on_message_) on_message_(self, in_msg_);
                        read_header();
                    }
                } else {
                    if (on_disconnect_) on_disconnect_(shared_from_this());
                    socket_.close();
                }
            });
    }

    void read_body() {
        asio::async_read(socket_,
            asio::buffer(in_msg_.body.data(), in_msg_.body.size()),
            [this, self = shared_from_this()](std::error_code ec, size_t) {
                if (!ec) {
                    if (on_message_) on_message_(self, in_msg_);
                    read_header();
                } else {
                    if (on_disconnect_) on_disconnect_(shared_from_this());
                    socket_.close();
                }
            });
    }

    void write_header() {
        asio::async_write(socket_,
            asio::buffer(&out_queue_.front().header, sizeof(MessageHeader)),
            [this, self = shared_from_this()](std::error_code ec, size_t) {
                if (!ec) {
                    if (out_queue_.front().body.size() > 0) {
                        write_body();
                    } else {
                        out_queue_.pop_front();
                        if (!out_queue_.empty()) write_header();
                    }
                } else {
                    if (on_disconnect_) on_disconnect_(shared_from_this());
                    socket_.close();
                }
            });
    }

    void write_body() {
        asio::async_write(socket_,
            asio::buffer(out_queue_.front().body.data(), out_queue_.front().body.size()),
            [this, self = shared_from_this()](std::error_code ec, size_t) {
                if (!ec) {
                    out_queue_.pop_front();
                    if (!out_queue_.empty()) write_header();
                } else {
                    if (on_disconnect_) on_disconnect_(shared_from_this());
                    socket_.close();
                }
            });
    }

    asio::ip::tcp::socket socket_;
    uint32_t id_;
    Message in_msg_;
    std::deque<Message> out_queue_;
    std::function<void(Ptr, Message)> on_message_;
    std::function<void(Ptr)> on_disconnect_;
};


class TcpServer {
public:
    explicit TcpServer(uint16_t port)
        : acceptor_(context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port))
    {}

    ~TcpServer() { stop(); }

    void start() {
        accept();
        thread_ = std::thread([this]() { context_.run(); });
    }

    void stop() {
        context_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void send_to(uint32_t client_id, const Message& msg) {
        for (auto& conn : connections_) {
            if (conn && conn->id() == client_id) {
                conn->send(msg);
                return;
            }
        }
    }

    void broadcast(const Message& msg) {
        for (auto& conn : connections_) {
            if (conn && conn->is_connected()) {
                conn->send(msg);
            }
        }
    }

    std::function<void(TcpConnection::Ptr)> on_connect;
    std::function<void(TcpConnection::Ptr)> on_disconnect;
    std::function<void(TcpConnection::Ptr, Message)> on_message;

private:
    void accept() {
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                auto conn = std::make_shared<TcpConnection>(
                    std::move(socket), next_id_++,
                    on_message,
                    [this](TcpConnection::Ptr c) {
                        if (on_disconnect) on_disconnect(c);
                        std::erase_if(connections_, [&](auto& p) { return p.get() == c.get(); });
                    }
                );
                connections_.push_back(conn);
                if (on_connect) on_connect(conn);
                conn->start();
            }
            accept();
        });
    }

    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    std::vector<TcpConnection::Ptr> connections_;
    uint32_t next_id_{1};
};

} // namespace ccbot::net
