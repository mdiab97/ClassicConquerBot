#pragma once
#include "common/net/message.h"
#include <asio.hpp>
#include <memory>
#include <deque>
#include <functional>
#include <thread>
#include <atomic>

namespace ccbot::net {

class TcpClient {
public:
    TcpClient() = default;

    ~TcpClient() { disconnect(); }

    bool connect(const std::string& host, uint16_t port) {
        try {
            asio::ip::tcp::resolver resolver(context_);
            auto endpoints = resolver.resolve(host, std::to_string(port));
            socket_ = std::make_unique<asio::ip::tcp::socket>(context_);
            asio::connect(*socket_, endpoints);
            connected_ = true;
            read_header();
            thread_ = std::thread([this]() { context_.run(); });
            return true;
        } catch (...) {
            return false;
        }
    }

    void disconnect() {
        connected_ = false;
        context_.stop();
        if (thread_.joinable()) thread_.join();
        if (socket_ && socket_->is_open()) socket_->close();
    }

    bool is_connected() const { return connected_; }

    void send(const Message& msg) {
        asio::post(context_, [this, msg]() {
            bool writing = !out_queue_.empty();
            out_queue_.push_back(msg);
            if (!writing) write_header();
        });
    }

    std::function<void(Message)> on_message;
    std::function<void()> on_disconnect;

private:
    void read_header() {
        asio::async_read(*socket_,
            asio::buffer(&in_msg_.header, sizeof(MessageHeader)),
            [this](std::error_code ec, size_t) {
                if (!ec) {
                    if (in_msg_.header.size > 0) {
                        in_msg_.body.resize(in_msg_.header.size);
                        read_body();
                    } else {
                        in_msg_.body.clear();
                        if (on_message) on_message(in_msg_);
                        read_header();
                    }
                } else {
                    connected_ = false;
                    if (on_disconnect) on_disconnect();
                }
            });
    }

    void read_body() {
        asio::async_read(*socket_,
            asio::buffer(in_msg_.body.data(), in_msg_.body.size()),
            [this](std::error_code ec, size_t) {
                if (!ec) {
                    if (on_message) on_message(in_msg_);
                    read_header();
                } else {
                    connected_ = false;
                    if (on_disconnect) on_disconnect();
                }
            });
    }

    void write_header() {
        asio::async_write(*socket_,
            asio::buffer(&out_queue_.front().header, sizeof(MessageHeader)),
            [this](std::error_code ec, size_t) {
                if (!ec) {
                    if (out_queue_.front().body.size() > 0) {
                        write_body();
                    } else {
                        out_queue_.pop_front();
                        if (!out_queue_.empty()) write_header();
                    }
                } else {
                    connected_ = false;
                    if (on_disconnect) on_disconnect();
                }
            });
    }

    void write_body() {
        asio::async_write(*socket_,
            asio::buffer(out_queue_.front().body.data(), out_queue_.front().body.size()),
            [this](std::error_code ec, size_t) {
                if (!ec) {
                    out_queue_.pop_front();
                    if (!out_queue_.empty()) write_header();
                } else {
                    connected_ = false;
                    if (on_disconnect) on_disconnect();
                }
            });
    }

    asio::io_context context_;
    std::unique_ptr<asio::ip::tcp::socket> socket_;
    std::thread thread_;
    Message in_msg_;
    std::deque<Message> out_queue_;
    std::atomic<bool> connected_{false};
};

} // namespace ccbot::net
