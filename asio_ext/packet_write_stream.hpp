#pragma once
#include <boost/asio.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/system/error_code.hpp>
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

    typedef void (*cb_type)(boost::system::error_code, std::size_t);

    using write_handler = std::function<void(boost::system::error_code const& ec, size_t bytes_transferred)>;

    struct option {
        // max size per next_layer send op
        size_t max_size_per_send = 64 * 1024;
    };

    template <typename T>
    using send_queue = detail::op_queue<T>;

    // Send Packet Type
    struct packet {
        PacketBuffer buf_;

        write_handler handler_;

        bool sending_ = false;

        bool half_ = false;

        packet* next_ = nullptr;

    public:
        void destroy() { delete this; }
    };

private:
    // next layer stream
    next_layer_type stream_;

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

    void close() {
        stream_.close();
    }

    void close(boost::system::error_code & ec) {
        stream_.close(ec);
    }

    /// ------------------- write_some
    template <typename ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers, boost::system::error_code & ec)
    {
        return boost::asio::write(stream_, buffers, ec);
    }

    template <typename ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers)
    {
        return boost::asio::write(stream_, buffers);
    }

    template <typename WriteHandler = cb_type>
        BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
            void (boost::system::error_code, std::size_t))
    async_write_some(PacketBuffer && buffer,
            BOOST_ASIO_MOVE_ARG(WriteHandler) handler = nullptr)
    {
        std::unique_ptr<packet> pack(new packet);
        buffer_adapter<PacketBuffer>::swap(pack->buf_, buffer);
        pack->handler_ = handler;

        boost::system::error_code ec = async_send_packet(pack);
        if (ec) {
            if (pack->handler_)
                post_handler(pack->handler_, ec, 0);
            return ;
        }

        pack.release();
    }

    template <typename ConstBufferSequence, typename WriteHandler = cb_type>
        BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
            void (boost::system::error_code, std::size_t))
    async_write_some(const ConstBufferSequence& buffers,
            BOOST_ASIO_MOVE_ARG(WriteHandler) handler = nullptr)
    {
        std::unique_ptr<packet> pack(new packet);
        for (auto it = buffer_sequence_begin(buffers); it != buffer_sequence_end(buffers); ++it) {
            const_buffer const& buf = *it;
            mutable_buffers_1 mbuf = buffer_adapter<PacketBuffer>::prepare(pack->buf_, buf.size());
            ::memcpy(mbuf.data(), buf.data(), buf.size());
            buffer_adapter<PacketBuffer>::commit(pack->buf_, buf.size());
        }
        pack->handler_ = handler;

        boost::system::error_code ec = async_send_packet(pack);
        if (ec) {
            if (pack->handler_)
                post_handler(pack->handler_, ec, 0);
            return ;
        }

        pack.release();
    }

    /// ------------------- read_some
    template <typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers) {
        return stream_.read_some(buffers);
    }

    template <typename MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers, boost::system::error_code & ec) {
        return stream_.read_some(buffers, ec);
    }

    template <typename MutableBufferSequence, typename ReadHandler>
        BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
            void (boost::system::error_code, std::size_t))
    async_read_some(const MutableBufferSequence& buffers,
        BOOST_ASIO_MOVE_ARG(ReadHandler) handler)
    {
        stream_.async_read_some(buffers, std::forward<ReadHandler>(handler));
    }

private:
    boost::system::error_code async_send_packet(std::unique_ptr<packet> & pack)
    {
        size_t bytes = buffer_adapter<PacketBuffer>::size(pack->buf_);
        std::unique_lock<std::mutex> lock(send_mutex_);
        if (ec_) return ec_;

        send_queue_.push(pack.get());
        buffered_bytes_ += bytes;
        flush();
        return boost::system::error_code();
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

        buffered_bytes_ = 0;
    }

    void handle_write(boost::system::error_code const& ec, size_t bytes)
    {
        std::unique_lock<std::mutex> lock(send_mutex_);
        this->sending_ = false;

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

        assert(buffered_bytes_ >= bytes);
        buffered_bytes_ -= bytes;

        flush();
    }

    void post_handler(write_handler const& handler, boost::system::error_code const& ec, std::size_t n) {
        if (handler) {
            get_io_context().post([handler, ec, n]()
                {
                    handler(boost::system::error_code(), ec, n);
                });
        }
    }
};

} //namespace asio_ext
