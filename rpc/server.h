#pragma once

#include <unordered_map>
#include <unordered_set>

#include <pthread.h>

#include "marshal.h"
#include "polling.h"

// for getaddrinfo() used in Server::start()
struct addrinfo;

namespace rpc {

class Server;

/**
 * The raw packet sent from client will be like this:
 * <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
 * NOTE: size does not include the size itself (<xid>..<argN>).
 *
 * For the request object, the marshal only contains <arg1>..<argN>,
 * other fields are already consumed.
 */
struct Request {
    Marshal m;
    i64 xid;
};

class Service {
public:
    virtual ~Service() {}
    virtual int __reg_to__(Server*) = 0;
};

class ServerConnection: public Pollable {
    friend class Server;

    virtual Marshal* output_buffer() = 0;
    virtual void close() = 0;

protected:

    Server* server_;
    int sock_;

public:
    ServerConnection(Server* server, int sock): server_(server), sock_(sock) {}
    virtual ~ServerConnection() {}
    virtual int fd() {
        return sock_;
    }

    // helper function, do some work in background
    int run_async(const std::function<void()>& f);

    virtual void begin_reply(Request* req, i32 error_code = 0) = 0;

    virtual void end_reply() = 0;

    void write_marshal(Marshal& m) {
        Marshal* buf = this->output_buffer();
        buf->read_from_marshal(m, m.content_size());
    }

    template<class T>
    ServerConnection& operator <<(const T& v) {
        Marshal* buf = this->output_buffer();
        *buf << v;
        return *this;
    }
};

class ServerUdpConnection: public ServerConnection {
    char* udp_buffer_;

    virtual void close() {
        // will not be called
        verify(0);
    }

    virtual Marshal* output_buffer() {
        // will not be called
        verify(0);
        return nullptr;
    }

public:
    ServerUdpConnection(Server* svr, int udp_sock): ServerConnection(svr, udp_sock) {
        udp_buffer_ = new char[UdpBuffer::max_udp_packet_size_s];
    }
    ~ServerUdpConnection() {
        delete[] udp_buffer_;
    }
    virtual int poll_mode() {
        return Pollable::READ;  // always read only
    }
    void handle_write() {
        // will not be called
        verify(0);
    }
    void handle_read();
    void handle_error() {
        ::close(sock_);
    }

    virtual void begin_reply(Request* req, i32 error_code = 0) {
        // no reply, should not be called
        verify(0);
    }

    virtual void end_reply() {
        // no reply, should not be called
        verify(0);
    }
};

class ServerTcpConnection: public ServerConnection {

    friend class Server;

    Marshal in_, out_;
    SpinLock out_l_;

    Marshal::bookmark* bmark_;

    enum {
        CONNECTED, CLOSED
    } status_;


    virtual Marshal* output_buffer() {
        return &out_;
    }

    /**
     * Only to be called by:
     * 1: ~Server(), which is called when destroying Server
     * 2: handle_error(), which is called by PollMgr
     */
    void close();

    // used to surpress multiple "no handler for rpc_id=..." errro
    static std::unordered_set<i32> rpc_id_missing_s;
    static SpinLock rpc_id_missing_l_s;

protected:

    // Protected destructor as required by RefCounted.
    ~ServerTcpConnection();

public:

    ServerTcpConnection(Server* server, int socket);

    /**
     * Start a reply message. Must be paired with end_reply().
     *
     * Reply message format:
     * <size> <xid> <error_code> <ret1> <ret2> ... <retN>
     * NOTE: size does not include size itself (<xid>..<retN>).
     *
     * User only need to fill <ret1>..<retN>.
     *
     * Currently used errno:
     * 0: everything is fine
     * ENOENT: method not found
     * EINVAL: invalid packet (field missing)
     */
    void begin_reply(Request* req, i32 error_code = 0);

    void end_reply();

    int poll_mode();
    void handle_write();
    void handle_read();
    void handle_error();
};

class DeferredReply: public NoCopy {
    rpc::Request* req_;
    rpc::ServerConnection* sconn_;
    std::function<void()> marshal_reply_;
    std::function<void()> cleanup_;

public:

    DeferredReply(rpc::Request* req, rpc::ServerConnection* sconn,
                  const std::function<void()>& marshal_reply, const std::function<void()>& cleanup)
        : req_(req), sconn_(sconn), marshal_reply_(marshal_reply), cleanup_(cleanup) {}

    ~DeferredReply() {
        cleanup_();
        delete req_;
        sconn_->release();
        req_ = nullptr;
        sconn_ = nullptr;
    }

    int run_async(const std::function<void()>& f) {
        return sconn_->run_async(f);
    }

    void reply() {
        sconn_->begin_reply(req_);
        marshal_reply_();
        sconn_->end_reply();
        delete this;
    }
};

class Server: public NoCopy {

    friend class ServerConnection;
    friend class ServerTcpConnection;
    friend class ServerUdpConnection;

    std::unordered_map<i32, std::function<void(Request*, ServerConnection*)>> handlers_;
    PollMgr* pollmgr_;
    ThreadPool* threadpool_;
    int server_sock_;

    bool udp_;
    int udp_sock_;

    ServerUdpConnection* udp_conn_;

    Counter sconns_ctr_;

    SpinLock sconns_l_;
    std::unordered_set<ServerConnection*> sconns_;

    enum {
        NEW, RUNNING, STOPPING, STOPPED
    } status_;

    pthread_t loop_th_;

    static void* start_server_loop(void* arg);
    void server_loop(struct addrinfo* svr_addr);

public:

    Server(PollMgr* pollmgr = nullptr, ThreadPool* thrpool = nullptr);
    virtual ~Server();

    void enable_udp() {
        udp_ = true;
    }

    int start(const char* bind_addr);

    int reg(Service* svc) {
        return svc->__reg_to__(this);
    }

    /**
     * The svc_func need to do this:
     *
     *  {
     *     // process request
     *     ..
     *
     *     // send reply
     *     server_connection->begin_reply();
     *     *server_connection << {reply_content};
     *     server_connection->end_reply();
     *
     *     // cleanup resource
     *     delete request;
     *     server_connection->release();
     *  }
     */
    int reg(i32 rpc_id, const std::function<void(Request*, ServerConnection*)>& func);

    template<class S>
    int reg(i32 rpc_id, S* svc, void (S::*svc_func)(Request*, ServerConnection*)) {

        // disallow duplicate rpc_id
        if (handlers_.find(rpc_id) != handlers_.end()) {
            return EEXIST;
        }

        handlers_[rpc_id] = [svc, svc_func] (Request* req, ServerConnection* sconn) {
            (svc->*svc_func)(req, sconn);
        };

        return 0;
    }

    void unreg(i32 rpc_id);
};

}

