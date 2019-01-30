#pragma once
#include <boost/asio.hpp>
#include <boost/asio/streambuf.hpp>
#include "buffer_adapter.hpp"
#include "async_guard.hpp"

namespace asio_ext {

using namespace boost::asio;

template <typename NextLayer,
          typename PacketBuffer = streambuf>
class packet_write_stream
{
public:
    /// The type of the next layer.
    using next_layer_type = typename std::remove_reference<NextLayer>::type;

    /// The type of the lowest layer.
    using lowest_layer_type = get_lowest_layer<next_layer_type>;

    /// The type of the executor associated with the object.
    using executor_type = typename next_layer_type::executor_type;

    using send_handler = std::function<void(boost::system::error_code const& ec, size_t bytes_transferred)>;

    struct option {
        // max packet size
        size_t max_packet_size = 64 * 1024;

        // max size per next_layer send op
        size_t max_size_per_send = 64 * 1024;
    };

    template <typename T>
    using send_queue = detail::op_queue<T>;

    // Send Packet Type
    struct packet {
        PacketBuffer buf_;

        send_handler handler_;

        bool sending_ = false;

        bool half_ = false;

        packet* next_ = nullptr;

    public:
        void destroy() { delete this; }
    };

private:
    // next layer stream
    next_layer_type stream_;

    // receive buffer
    streambuf recv_buffer_;

    // send packets list
    send_queue<packet> send_queue_;

    // send lock
    std::mutex send_mutex_;

    // buffered bytes
    size_t buffered_bytes_ = 0;

    // is sending
    bool sending_ = false;

    option opt_;

    boost::system::error_code ec_;

    // async callback lock
    async_guard_ptr async_guard_;

public:
    template <typename ... Args>
    explicit packet_write_stream(Args && ... args)
        : stream_(std::forward<Args>(args)...), async_guard_(async_guard::create())
    {
    }

    virtual ~packet_write_stream()
    {
        async_guard_->cancel();
    }

    void set_option(option const& opt) {
        opt_ = opt;
    }

    io_context& get_io_context() {
        return lowest_layer().get_io_context();
    }

    next_layer_type & next_layer() {
        return stream_;
    }

    next_layer_type const& next_layer() const {
        return stream_;
    }

    lowest_layer_type & lowest_layer() {
        return stream_.lowest_layer();
    }

    lowest_layer_type const& lowest_layer() const {
        return stream_.lowest_layer();
    }

    /// ------------------- send/write
    template <typename ConstBufferSequence>
    std::size_t send(const ConstBufferSequence& buffers, boost::system::error_code & ec)
    {
        if (ec_) {
            ec = ec_;
            return ;
        }

        std::unique_ptr<packet> pack(new packet);
        for (auto it = buffer_sequence_begin(buffers); it != buffer_sequence_end(buffers); ++it) {
            const_buffer const& buf = *it;
            mutable_buffers_1 mbuf = buffer_adapter<PacketBuffer>::prepare(pack->buf_, buf.size());
            ::memcpy(mbuf.data(), buf.data(), buf.size());
            buffer_adapter<PacketBuffer>::commit(pack->buf_, buf.size());
        }
        return send(pack.release());
    }

    template <typename ConstBufferSequence>
    std::size_t send(const ConstBufferSequence& buffers)
    {
        boost::system::error_code ec;
        std::size_t bytes_transferred = send(buffers, ec);
        if(ec)
            BOOST_THROW_EXCEPTION(system_error{ec});
        return bytes_transferred;
    }

    std::size_t send(PacketBuffer && buffer, boost::system::error_code & ec)
    {
        if (ec_) {
            ec = ec_;
            return ;
        }

        std::unique_ptr<packet> pack(new packet);
        buffer_adapter<PacketBuffer>::swap(pack->buf_, buffer);
        return send(pack.release());
    }

    template<class ConstBufferSequence>
    std::size_t write(ConstBufferSequence const& buffers) {
        return send(buffers);
    }

    template<class ConstBufferSequence>
    std::size_t write(ConstBufferSequence const& buffers, error_code& ec) {
        return send(buffers, ec);
    }

    template<class ConstBufferSequence>
    std::size_t write_some(ConstBufferSequence const& buffers) {
        return send(buffers);
    }

    template<class ConstBufferSequence>
    std::size_t write_some(ConstBufferSequence const& buffers, error_code& ec) {
        return send(buffers, ec);
    }

    template <typename ConstBufferSequence, typename WriteHandler>
        BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
            void (boost::system::error_code, std::size_t))
    async_write_some(const ConstBufferSequence& buffers,
            BOOST_ASIO_MOVE_ARG(WriteHandler) handler)
    {
        if (ec_) {
            post_handler(handler, ec_, 0);
            return ;
        }

        std::unique_ptr<packet> pack(new packet);
        for (auto it = buffer_sequence_begin(buffers); it != buffer_sequence_end(buffers); ++it) {
            const_buffer const& buf = *it;
            mutable_buffers_1 mbuf = buffer_adapter<PacketBuffer>::prepare(pack->buf_, buf.size());
            ::memcpy(mbuf.data(), buf.data(), buf.size());
            buffer_adapter<PacketBuffer>::commit(pack->buf_, buf.size());
        }
        pack.handler_ = handler;
        return send(pack.release());
    }

    template <typename ConstBufferSequence, typename WriteHandler>
        BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
            void (boost::system::error_code, std::size_t))
    async_send(const ConstBufferSequence& buffers,
        BOOST_ASIO_MOVE_ARG(WriteHandler) handler)
    {
        return async_write_some(buffers, std::forward<WriteHandler>(handler));
    }

    /// ------------------- receive/read
    template <typename MutableBufferSequence>
    std::size_t receive(const MutableBufferSequence& buffers) {
        return stream_.receive(buffers);
    }

    template <typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers) {
        return stream_.read_some(buffers);
    }

private:
    std::size_t send(packet * pack)
    {
        size_t bytes = buffer_adapter<PacketBuffer>::size(pack->buf_);
        std::unique_lock<std::mutex> lock(send_mutex_);
        send_queue_.push(pack);
        buffered_bytes_ += bytes;
        flush();
        return bytes;
    }

    void flush()
    {
        if (sending_) return ;

        packet* pack = send_queue_.front();

        const_buffers_1 buffers[128];
        size_t count = 0;
        size_t bytes = 0;
        while (count < sizeof(buffers)/sizeof(buffers[0])
            && bytes < opt_.max_size_per_send
            && pack)
        {
            const_buffers_1 buf = buffer_adapter<PacketBuffer>::data(pack->buf_);
            bytes += buf.size();
            std::swap(buf, buffers[count]);
            ++count;
            pack->sending_ = true;
            pack = pack->next_;
        }

        if (!count) return ;

        sending_ = true;
        auto async_guard = async_guard_;
        stream_.async_write_some(buffer(buffers, count), [this, async_guard](boost::system::error_code const& ec, size_t bytes)
                {
                    async_scoped scoped(async_guard);
                    if (!scoped)
                        return ;

                    this->handle_write(ec, bytes);
                });
    }

    void handle_error(boost::system::error_code const& ec) {
        if (!ec_) ec_ = ec;

        packet* pack = send_queue_.front();
        while (pack) {
            post_handler(pack->handler_, ec_, 0);
            send_queue_.pop();
            pack->destroy();
            pack = send_queue_.front();
        }
    }

    void handle_write(boost::system::error_code const& ec, size_t bytes)
    {
        std::unique_lock<std::mutex> lock(send_mutex_);

        if (ec) {
            handle_error(ec);
            return ;
        }

        size_t consumed = bytes;
        packet* pack = send_queue_.front();
        while (consumed) {
            assert(pack);
            size_t n = buffer_adapter<PacketBuffer>::size(pack->buf_);
            if (consumed >= n) {
                post_handler(pack->handler_, boost::system::error_code(), n);
                send_queue_.pop();
                pack->destroy();
                pack = send_queue_.front();
                consumed -= n;
            } else {
                pack->half_ = true;
                buffer_adapter<PacketBuffer>::consume(pack->buf_, consumed);
                consumed = 0;
            }
        }

        this->sending_ = false;
        flush();
    }

    void post_handler(send_handler const& handler, boost::system::error_code const& ec, std::size_t n) {
        if (handler) {
            get_io_context().post([handler, ec, n]()
                {
                    handler(boost::system::error_code(), ec, n);
                });
        }
    }
};

} //namespace asio_ext
