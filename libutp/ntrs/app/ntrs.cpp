#include <errno.h>
#include <cassert>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/util.h>
#include <ntrs/service.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <utils/CLI11.hpp>
#include <utp/nat.h>

#include "app_log.h"
#include "mpscq.h"
#include "proto/frame.h"
#include "proto/proto.h"
#include "rendezvous/rendezvous.h"
#include "service_util.h"

#define fprintf(stream, ...) UTP_NTRS_APP_LOG(stream, __VA_ARGS__)

struct Registration {
    utp_ntrs_endpoint_t                                            endpoint;
    utp_ntrs_endpoint_t                                            calibration_ip;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    token;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES> local_candidates;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES>  observed_addresses;
    std::array<uint16_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES + 1u> calibration_ports;
    std::array<uint64_t, UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES>       observed_at_unix_ms;
    utp_address_t                                                  reported_public_endpoint;
    uint64_t                                                       last_register_request_id;
    uint64_t                                                       last_activity_ms;
    uint64_t                                                       keepalive_packet_number;
    uint64_t                                                       last_keepalive_ping_ms;
    uint16_t                                                       local_port;
    uint8_t                                                        nat_class;
    uint8_t                                                        local_family;
    uint8_t                                                        local_candidate_count;
    uint8_t                                                        observed_address_count;
    uint8_t                                                        calibration_endpoint_count;
    uint8_t                                                        calibration_received_mask;
    bool                                                           has_reported_public_endpoint;
    bool                                                           last_register_was_initial;
    bool                                                           calibration_ip_consistent;
    uint64_t                                                       calibration_id;

    bool    matchesToken(const uint8_t value[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE]) const;
    bool    acceptsFamily(const utp_ntrs_endpoint_t& observed) const;
    void    touch(uint64_t now_ms);
    void    updateEndpoint(const utp_ntrs_endpoint_t& observed);
    void    update(const utp_rendezvous_register_t& request, const utp_ntrs_endpoint_t& observed);
    uint8_t updateObservedAddresses(const utp_rendezvous_address_update_t& update);
    void    recordCalibrationEndpoint(uint8_t index, const utp_ntrs_endpoint_t& observed);
};

struct PendingRendezvous {
    utp_ntrs_endpoint_t                                            source_endpoint;
    utp_ntrs_endpoint_t                                            target_endpoint;
    sockaddr_storage                                               source_socket_address;
    sockaddr_storage                                               target_socket_address;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    target_token;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    calibration_token;
    std::array<uint8_t, UTP_RENDEZVOUS_PUNCH_TOKEN_SIZE>           punch_token;
    std::array<uint8_t, UTP_RENDEZVOUS_ID_SIZE>                    rendezvous_id;
    std::array<uint8_t, UTP_PEER_ID_MAX_LENGTH>                    source_peer_id;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES> source_local_candidates;
    std::array<uint16_t, 3u>                                       calibration_ports;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR>                      forward_packet;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR>                      redirect_body;
    utp_address_t                                                  source_reported_public_endpoint;
    utp_ntrs_endpoint_t                                            calibration_ip;
    uint64_t                                                       created_at_ms;
    uint64_t                                                       calibration_id;
    uint64_t                                                       calibration_deadline_ms;
    uint64_t                                                       forward_packet_number;
    uint64_t                                                       forward_retry_at_ms;
    size_t                                                         forward_packet_length;
    size_t                                                         redirect_body_length;
    socklen_t                                                      source_socket_length;
    socklen_t                                                      target_socket_length;
    uint16_t                                                       source_local_port;
    uint8_t                                                        source_peer_id_length;
    uint8_t                                                        source_local_family;
    uint8_t                                                        source_local_candidate_count;
    uint8_t                                                        calibration_received_mask;
    uint8_t                                                        forward_retries_remaining;
    uint32_t                                                       forward_retry_delay_ms;
    bool                                                           has_source_reported_public_endpoint;
    bool                                                           calibration_ip_consistent;
    bool                                                           calibration_pending;
    bool                                                           forward_delivered;
};

// Worker owns PendingRendezvous; the control thread owns the short-lived
// calibration session and sends the result back to the originating Worker.
struct CalibrationStartTask {
    std::array<uint8_t, UTP_RENDEZVOUS_ID_SIZE>                    rendezvous_id;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    calibration_token;
    utp_ntrs_endpoint_t                                            observed;
    uint64_t                                                        calibration_id;
    uint64_t                                                        deadline_ms;
    uint16_t                                                        owner_worker;
};

struct CalibrationSession {
    std::array<uint8_t, UTP_RENDEZVOUS_ID_SIZE>                    rendezvous_id;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>    calibration_token;
    utp_ntrs_endpoint_t                                            calibration_ip;
    std::array<uint16_t, 3u>                                       calibration_ports;
    uint64_t                                                        calibration_id;
    uint64_t                                                        deadline_ms;
    uint64_t                                                        completed_at_ms;
    uint16_t                                                        owner_worker;
    uint8_t                                                         received_mask;
    bool                                                            ip_consistent;
    bool                                                            completed;
    bool                                                            ready_delivered;
    bool                                                            result_delivered;
};

class NtrsServer;

struct OutgoingDatagram {
    sockaddr_storage                          peer;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR> packet;
    size_t                                    packet_length;
    socklen_t                                 peer_length;
};

class NtrsServer;

struct UdpSocket {
    NtrsServer*                  server;
    event*                       read_event;
    event*                       write_event;
    std::deque<OutgoingDatagram> output_queue;
    MpscQueue<OutgoingDatagram>  control_output_queue;
    std::atomic<size_t>          control_output_queue_size;
    int                          fd;
    int                          control_output_read_fd;
    int                          control_output_write_fd;
    event*                       control_output_event;
    uint8_t                      calibration_index;
    bool                         write_event_active;
};

struct PreparedDatagram {
    UdpSocket*                                socket;
    sockaddr_storage                          peer;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR> packet;
    size_t                                    packet_length;
    socklen_t                                 peer_length;
};

struct ControlTask {
    sockaddr_storage                          peer;
    utp_ntrs_endpoint_t                       observed;
    utp_packet_header_t                       header;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR> frame_payload;
    UdpSocket*                                socket;
    socklen_t                                 peer_length;
    uint16_t                                  frame_payload_length;
    uint8_t                                   message_type;
};

enum WorkerTaskType {
    kWorkerTaskRequest              = 1u,
    kWorkerTaskForwardPong         = 2u,
    kWorkerTaskCalibrationAccepted = 3u
};

struct WorkerTask {
    sockaddr_storage                          peer;
    utp_ntrs_endpoint_t                       observed;
    utp_packet_header_t                       header;
    std::array<uint8_t, UTP_PACKET_MTU_FLOOR> frame_payload;
    UdpSocket*                                socket;
    socklen_t                                 peer_length;
    uint16_t                                  frame_payload_length;
    uint8_t                                   message_type;
    uint8_t                                   task_type;
    std::array<uint8_t, UTP_RENDEZVOUS_ID_SIZE>                 rendezvous_id;
    std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE> calibration_token;
    utp_ntrs_endpoint_t                                         calibration_ip;
    std::array<uint16_t, 3u>                                    calibration_ports;
    uint64_t                                                     calibration_id;
    uint8_t                                                      calibration_received_mask;
    bool                                                         calibration_ip_consistent;
    bool                                                         calibration_complete;
};

struct SharedNtrsState {
    std::mutex                                    mutex;
    std::atomic<uint64_t>                         next_packet_number;
    std::atomic<bool>                             fatal_socket_error;
    std::atomic<int>                              fatal_error_code;
    MpscQueue<ControlTask>                        lifecycle_control_queue;
    MpscQueue<ControlTask>                        keepalive_control_queue;
    MpscQueue<CalibrationStartTask>               calibration_start_queue;
    int                                           control_notify_fd;
    int                                           control_notify_write_fd;
    std::unordered_map<std::string, Registration> registrations;
    std::unordered_map<std::string, uint64_t>     unregistration_tombstones;
    std::unordered_set<std::string>               active_registration_tokens;
    // Registration state is owned by the control thread. This index routes
    // forward acknowledgements to the Worker that owns a rendezvous.
    std::unordered_map<std::string, uint16_t>     forward_owners;
    std::vector<NtrsServer*>                      workers;

    SharedNtrsState()
        : next_packet_number(1u),
          fatal_socket_error(false),
          fatal_error_code(0),
          lifecycle_control_queue(),
          keepalive_control_queue(),
          calibration_start_queue(),
          control_notify_fd(-1),
          control_notify_write_fd(-1)
    {
    }

    uint64_t nextPacketNumber()
    {
        uint64_t current = next_packet_number.load(std::memory_order_relaxed);

        while (current != 0u && current <= UTP_PACKET_NUMBER_MAX) {
            if (next_packet_number.compare_exchange_weak(current, current + 1u, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
                return current;
            }
        }
        return 0u;
    }
};

struct OutgoingFrame;

class NtrsServer
{
public:
    explicit NtrsServer(const std::shared_ptr<SharedNtrsState>& shared_state, uint16_t worker_index);
    ~NtrsServer();

    bool start(const utp_ntrs_endpoint_t& bind_endpoint, const utp_ntrs_endpoint_t& advertised_endpoint,
               const char* interface_name, const std::vector<uint16_t>& calibration_ports,
               uint8_t public_candidate_count, uint32_t registration_timeout_ms, uint32_t keepalive_interval_ms,
               size_t output_queue_capacity, size_t worker_queue_capacity);
    int  run(const utp_ntrs_endpoint_t& endpoint);

private:
    friend class NtrsRuntime;

    NtrsServer(const NtrsServer&)            = delete;
    NtrsServer& operator=(const NtrsServer&) = delete;

    static void OnReadEvent(evutil_socket_t, short, void* user_data);
    static void OnControlEvent(evutil_socket_t, short, void* user_data);
    static void OnControlOutputEvent(evutil_socket_t, short, void* user_data);
    static void OnWorkerEvent(evutil_socket_t, short, void* user_data);
    static void OnTimerEvent(evutil_socket_t, short, void* user_data);
    static void OnControlTimerEvent(evutil_socket_t, short, void* user_data);
    static void OnWriteEvent(evutil_socket_t, short, void* user_data);

    void        stopForFatalError(int error, const char* operation);
    void        releaseSocket(UdpSocket* socket);
    bool        createSocket(UdpSocket* socket, const utp_ntrs_endpoint_t& endpoint, const char* interface_name,
                             uint8_t calibration_index, event_base* event_loop);
    bool sendDatagram(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length, const uint8_t* packet,
                      size_t packet_length);
    bool sendDatagram(const PreparedDatagram& datagram);
    bool encodeFrames(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                      const OutgoingFrame* frames, size_t frame_count, uint64_t* packet_number,
                      PreparedDatagram* encoded_datagram);
    bool sendFrames(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length, const OutgoingFrame* frames,
                    size_t frame_count, uint64_t* packet_number,
                    std::array<uint8_t, UTP_PACKET_MTU_FLOOR>* encoded_packet        = NULL,
                    size_t*                                    encoded_packet_length = NULL);
    bool sendMessage(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length, uint8_t message_type,
                     const uint8_t* body, size_t body_length);
    bool sendRejected(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                      uint8_t rejected_message_type, const uint8_t* reference_id, uint8_t reference_length,
                      uint16_t reason_code);
    bool prepareKeepalivePing(const Registration& registration, PreparedDatagram* datagram, uint64_t* packet_number);
    bool beginCalibration(Registration* registration);
    bool prepareCalibration(const PendingRendezvous& transaction, PreparedDatagram* datagram);
    bool preparePendingRendezvous(PendingRendezvous* transaction, PreparedDatagram* redirect,
                                  PreparedDatagram* forward);
    void expireRegistrations(uint64_t now_ms);
    void handleRegister(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                        const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame);
    void handlePing(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                    const utp_ntrs_endpoint_t& observed, const utp_packet_header_t& header,
                    const utp_frame_rendezvous_t& frame);
    void handlePong(const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame);
    void handleAddressUpdate(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                             const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame);
    void handleUnregister(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                          const utp_frame_rendezvous_t& frame);
    void handleRequest(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                       const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame);
    void handleCalibrationPing(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                               const utp_ntrs_endpoint_t& observed, const utp_packet_header_t& header,
                               const utp_frame_rendezvous_t& frame);
    void handleCalibrationStart(const CalibrationStartTask& task);
    void handleCalibrationAccepted(const WorkerTask& task);
    bool enqueueCalibrationResult(const CalibrationSession& session);
    void handleForwardPong(const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame);
    void erasePendingRendezvous(const std::string& rendezvous_key);
    bool isRegistrationTokenActive(const std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>& token) const;
    bool enqueueControl(const ControlTask& task);
    bool enqueueCalibrationStart(const CalibrationStartTask& task);
    bool enqueueControlOutput(const PreparedDatagram& datagram);
    bool enqueueWorker(const WorkerTask& task);
    void handleControlTask(const ControlTask& task);
    void onControlEvent();
    void onControlOutput(UdpSocket* socket);
    void onWorkerEvent();
    void onControlTimer();
    int  runControl();
    void onRead(UdpSocket* socket);
    void onTimer();
    void onWrite(UdpSocket* socket);

    event_base*                                                    base_;
    event*                                                         timer_event_;
    event*                                                         control_event_;
    event*                                                         control_timer_event_;
    event*                                                         worker_event_;
    event_base*                                                    control_base_;
    UdpSocket                                                      primary_socket_;
    std::array<UdpSocket, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES>     calibration_sockets_;
    std::array<utp_address_t, UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES> calibration_endpoints_;
    std::shared_ptr<SharedNtrsState>                               shared_state_;
    MpscQueue<WorkerTask>                                          worker_queue_;
    int                                                            worker_notify_fd_;
    int                                                            worker_notify_write_fd_;
    uint8_t                                                        calibration_endpoint_count_;
    uint8_t                                                        public_candidate_count_;
    uint32_t                                                       registration_timeout_ms_;
    uint32_t                                                       keepalive_interval_ms_;
    size_t                                                         output_queue_capacity_;
    uint16_t                                                       worker_index_;
    bool                                                           fatal_socket_error_;
    std::unordered_map<std::string, PendingRendezvous>             pending_rendezvous_;
    std::unordered_map<std::string, CalibrationSession>             calibration_sessions_;
    utp_ntrs_endpoint_t                                             bind_endpoint_;
    utp_ntrs_endpoint_t                                             advertised_endpoint_;
    std::string                                                      interface_name_;
    std::vector<uint16_t>                                           calibration_ports_;
};

static const uint64_t        k_pending_rendezvous_lifetime_ms       = 30000u;
static const uint64_t        k_unregistration_tombstone_lifetime_ms = 30000u;
static const uint32_t        k_registration_timeout_default_ms      = 90000u;
static const uint32_t        k_keepalive_interval_default_ms        = 30000u;
static const uint32_t        k_forward_retry_initial_delay_ms       = 1000u;
static const uint32_t        k_forward_retry_max_delay_ms           = 8000u;
static const uint32_t        k_forward_retry_send_failure_delay_ms  = 100u;
static const uint8_t         k_forward_retry_count                  = 3u;
static const uint32_t        k_temporary_calibration_timeout_ms     = 1000u;
static const uint8_t         k_temporary_calibration_endpoint_count = 2u;
static const size_t          k_output_queue_capacity_default        = 128u;
static const size_t          k_control_queue_capacity_default       = 1024u;
static const size_t          k_worker_queue_capacity_default        = 1024u;
static const uint8_t         k_primary_socket_index                 = UINT8_MAX;
static volatile sig_atomic_t g_stop_requested                       = 0;

enum NtrsRejectionReason {
    k_rejection_peer_not_found = 2u,
    k_rejection_peer_id_exists = 3u,
    k_rejection_token_invalid  = 4u
};

static bool is_fatal_udp_send_error(int error)
{
#ifdef EBADF
    if (error == EBADF) return true;
#endif
#ifdef ENOTSOCK
    if (error == ENOTSOCK) return true;
#endif
#ifdef EINVAL
    if (error == EINVAL) return true;
#endif
#ifdef ENOBUFS
    if (error == ENOBUFS) return true;
#endif
    return false;
}

NtrsServer::NtrsServer(const std::shared_ptr<SharedNtrsState>& shared_state, uint16_t worker_index)
    : base_(NULL),
      timer_event_(NULL),
      control_event_(NULL),
      control_timer_event_(NULL),
      worker_event_(NULL),
      control_base_(NULL),
      primary_socket_(),
      calibration_sockets_(),
      calibration_endpoints_(),
      shared_state_(shared_state),
      worker_queue_(),
      worker_notify_fd_(-1),
      worker_notify_write_fd_(-1),
      calibration_endpoint_count_(0u),
      public_candidate_count_(UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES),
      registration_timeout_ms_(k_registration_timeout_default_ms),
      keepalive_interval_ms_(k_keepalive_interval_default_ms),
      output_queue_capacity_(k_output_queue_capacity_default),
      worker_index_(worker_index),
      fatal_socket_error_(false),
      pending_rendezvous_(),
      calibration_sessions_(),
      bind_endpoint_(),
      advertised_endpoint_(),
      interface_name_(),
      calibration_ports_()
{
    primary_socket_.fd                      = -1;
    primary_socket_.control_output_read_fd  = -1;
    primary_socket_.control_output_write_fd = -1;
    primary_socket_.control_output_queue_size.store(0u, std::memory_order_relaxed);
    for (size_t index = 0u; index < calibration_sockets_.size(); ++index) {
        calibration_sockets_[index].fd                      = -1;
        calibration_sockets_[index].control_output_read_fd  = -1;
        calibration_sockets_[index].control_output_write_fd = -1;
        calibration_sockets_[index].control_output_queue_size.store(0u, std::memory_order_relaxed);
    }
}

NtrsServer::~NtrsServer()
{
    WorkerTask task = {};
    while (worker_queue_.pop(&task)) {
    }
    for (size_t index = 0u; index < calibration_sockets_.size(); ++index) releaseSocket(&calibration_sockets_[index]);
    releaseSocket(&primary_socket_);
    if (control_event_ != NULL) event_free(control_event_);
    if (control_timer_event_ != NULL) event_free(control_timer_event_);
    if (worker_event_ != NULL) event_free(worker_event_);
    if (control_base_ != NULL) event_base_free(control_base_);
    if (timer_event_ != NULL) event_free(timer_event_);
    if (base_ != NULL) event_base_free(base_);
    if (worker_notify_fd_ >= 0) close(worker_notify_fd_);
    if (worker_notify_write_fd_ >= 0) close(worker_notify_write_fd_);
}

void NtrsServer::stopForFatalError(int error, const char* operation)
{
    assert(operation != NULL);
    if (fatal_socket_error_) return;
    fatal_socket_error_ = true;
    assert(shared_state_ != NULL);
    int expected = 0;
    if (shared_state_->fatal_error_code.compare_exchange_strong(expected, error, std::memory_order_relaxed,
                                                                std::memory_order_relaxed)) {
        fprintf(stderr, "NTRS [Fatal] operation=%s errno=%d description=%s\n", operation, error, strerror(error));
    }
    shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
    if (base_ != NULL) event_base_loopbreak(base_);
}

void NtrsServer::releaseSocket(UdpSocket* socket)
{
    assert(socket != NULL);
    OutgoingDatagram datagram = {};
    while (socket->control_output_queue.pop(&datagram)) {
    }
    if (socket->control_output_event != NULL) event_free(socket->control_output_event);
    if (socket->write_event != NULL) event_free(socket->write_event);
    if (socket->read_event != NULL) event_free(socket->read_event);
    if (socket->control_output_read_fd >= 0) close(socket->control_output_read_fd);
    if (socket->control_output_write_fd >= 0) close(socket->control_output_write_fd);
    if (socket->fd >= 0) close(socket->fd);
    socket->output_queue.clear();
    socket->server                  = NULL;
    socket->read_event              = NULL;
    socket->write_event             = NULL;
    socket->control_output_event    = NULL;
    socket->fd                      = -1;
    socket->control_output_read_fd  = -1;
    socket->control_output_write_fd = -1;
    socket->calibration_index       = 0u;
    socket->write_event_active      = false;
    socket->control_output_queue_size.store(0u, std::memory_order_relaxed);
}

bool NtrsServer::createSocket(UdpSocket* socket, const utp_ntrs_endpoint_t& endpoint, const char* interface_name,
                              uint8_t calibration_index, event_base* event_loop)
{
    sockaddr_storage socket_address = {};
    socklen_t        socket_length  = 0u;
    int              socket_pair[2] = {-1, -1};

    assert(socket != NULL);
    assert(event_loop != NULL);
    if (!utp_ntrs_endpoint_to_sockaddr(&endpoint, &socket_address, &socket_length)) return false;
    socket->server                  = this;
    socket->read_event              = NULL;
    socket->write_event             = NULL;
    socket->control_output_event    = NULL;
    socket->fd                      = -1;
    socket->control_output_read_fd  = -1;
    socket->control_output_write_fd = -1;
    socket->write_event_active      = false;
    socket->control_output_queue_size.store(0u, std::memory_order_relaxed);
    socket->calibration_index = calibration_index;
    if (!socket->control_output_queue.create(output_queue_capacity_)) return false;
    const int reuse_port = 1;
    if ((socket->fd = ::socket(endpoint.family, SOCK_DGRAM, 0)) < 0 ||
        setsockopt(socket->fd, SOL_SOCKET, SO_REUSEPORT, &reuse_port, sizeof(reuse_port)) != 0 ||
        !utp_ntrs_socket_bind_interface(socket->fd, interface_name) ||
        bind(socket->fd, reinterpret_cast<const sockaddr*>(&socket_address), socket_length) != 0 ||
        evutil_make_socket_nonblocking(socket->fd) != 0 || socketpair(AF_UNIX, SOCK_DGRAM, 0, socket_pair) != 0) {
        releaseSocket(socket);
        if (socket_pair[0] >= 0) close(socket_pair[0]);
        if (socket_pair[1] >= 0) close(socket_pair[1]);
        return false;
    }
    if (evutil_make_socket_nonblocking(socket_pair[0]) != 0 || evutil_make_socket_nonblocking(socket_pair[1]) != 0) {
        close(socket_pair[0]);
        close(socket_pair[1]);
        socket_pair[0] = -1;
        socket_pair[1] = -1;
        releaseSocket(socket);
        return false;
    }
    socket->control_output_read_fd  = socket_pair[0];
    socket->control_output_write_fd = socket_pair[1];
    socket_pair[0]                  = -1;
    socket_pair[1]                  = -1;
    if ((socket->read_event = event_new(event_loop, socket->fd, EV_READ | EV_PERSIST, OnReadEvent, socket)) == NULL ||
        (socket->write_event = event_new(event_loop, socket->fd, EV_WRITE | EV_PERSIST, OnWriteEvent, socket)) == NULL ||
        (socket->control_output_event = event_new(event_loop, socket->control_output_read_fd, EV_READ | EV_PERSIST,
                                                  OnControlOutputEvent, socket)) == NULL ||
        event_add(socket->control_output_event, NULL) != 0 || event_add(socket->read_event, NULL) != 0) {
        releaseSocket(socket);
        return false;
    }
    return true;
}

static bool token_is_zero(const uint8_t* token)
{
    uint8_t value = 0u;
    for (size_t index = 0u; index < UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE; ++index) value |= token[index];
    return value == 0u;
}

static bool random_token(std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>* token)
{
    assert(token != NULL);
    size_t offset = 0u;
    while (offset < token->size()) {
        const ssize_t count = ::getrandom(token->data() + offset, token->size() - offset, 0);
        if (count > 0)
            offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return !token_is_zero(token->data());
}

static bool random_u64(uint64_t* value)
{
    std::array<uint8_t, sizeof(*value)> bytes = {};

    assert(value != NULL);
    if (!random_token(&bytes)) return false;
    memcpy(value, bytes.data(), sizeof(*value));
    return *value != 0u;
}

static bool random_port(uint16_t* port)
{
    uint16_t value = 0u;
    ssize_t  count;

    assert(port != NULL);
    do {
        do {
            count = ::getrandom(&value, sizeof(value), 0);
        } while (count < 0 && errno == EINTR);
        if (count != static_cast<ssize_t>(sizeof(value))) return false;
    } while (value == 0u);
    *port = value;
    return true;
}

static bool endpoint_to_address(const utp_ntrs_endpoint_t& endpoint, utp_address_t* address)
{
    size_t address_length = 0u;

    if (endpoint.family == AF_INET) {
        address->family = UTP_ADDRESS_FAMILY_IPV4;
        address_length  = 4u;
    } else if (endpoint.family == AF_INET6) {
        address->family = UTP_ADDRESS_FAMILY_IPV6;
        address_length  = 16u;
    } else {
        return false;
    }
    address->port = endpoint.port;
    memcpy(address->address, endpoint.address, address_length);
    return endpoint.port != 0u;
}

static bool endpoint_is_unspecified(const utp_ntrs_endpoint_t& endpoint)
{
    const size_t address_length = endpoint.family == AF_INET ? 4u : endpoint.family == AF_INET6 ? 16u : 0u;

    if (address_length == 0u) return true;
    for (size_t index = 0u; index < address_length; ++index) {
        if (endpoint.address[index] != 0u) return false;
    }
    return true;
}

static bool endpoints_equal(const utp_ntrs_endpoint_t& left, const utp_ntrs_endpoint_t& right)
{
    const size_t address_length = left.family == AF_INET ? 4u : left.family == AF_INET6 ? 16u : 0u;

    return address_length != 0u && left.family == right.family && left.port == right.port &&
           memcmp(left.address, right.address, address_length) == 0;
}

static bool endpoint_ips_equal(const utp_ntrs_endpoint_t& left, const utp_ntrs_endpoint_t& right)
{
    const size_t address_length = left.family == AF_INET ? 4u : left.family == AF_INET6 ? 16u : 0u;

    return address_length != 0u && left.family == right.family &&
           memcmp(left.address, right.address, address_length) == 0;
}

static bool addresses_equal(const utp_address_t& left, const utp_address_t& right)
{
    const size_t address_length = left.family == UTP_ADDRESS_FAMILY_IPV4   ? 4u
                                  : left.family == UTP_ADDRESS_FAMILY_IPV6 ? 16u
                                                                           : 0u;

    return address_length != 0u && left.family == right.family && left.port == right.port &&
           memcmp(left.address, right.address, address_length) == 0;
}

static bool address_matches_endpoint_ip(const utp_address_t& address, const utp_ntrs_endpoint_t& endpoint)
{
    const size_t  address_length  = address.family == UTP_ADDRESS_FAMILY_IPV4   ? 4u
                                    : address.family == UTP_ADDRESS_FAMILY_IPV6 ? 16u
                                                                                : 0u;
    const uint8_t endpoint_family = endpoint.family == AF_INET    ? static_cast<uint8_t>(UTP_ADDRESS_FAMILY_IPV4)
                                    : endpoint.family == AF_INET6 ? static_cast<uint8_t>(UTP_ADDRESS_FAMILY_IPV6)
                                                                  : 0u;

    return address_length != 0u && address.family == endpoint_family &&
           memcmp(address.address, endpoint.address, address_length) == 0;
}

static bool append_public_candidate(utp_rendezvous_candidate_plan_t* plan, const utp_address_t& candidate)
{
    for (uint8_t index = 0u; index < plan->public_candidate_count; ++index) {
        if (addresses_equal(plan->decoded_public_candidates[index], candidate)) return true;
    }
    if (plan->public_candidate_count >= UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES) return false;
    plan->decoded_public_candidates[plan->public_candidate_count++] = candidate;
    return true;
}

static bool make_candidate_plan(utp_rendezvous_candidate_plan_t* plan, uint8_t family, uint16_t local_port,
                                const utp_address_t* local_candidates, uint8_t local_candidate_count,
                                const utp_address_t*       reported_public_endpoint,
                                const utp_ntrs_endpoint_t& observed_endpoint, uint8_t public_candidate_count)
{
    utp_address_t observed_public_endpoint = {};

    assert(plan != NULL);
    if (public_candidate_count == 0u || public_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        local_port == 0u || local_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        !endpoint_to_address(observed_endpoint, &observed_public_endpoint) ||
        observed_public_endpoint.family != family || (local_candidate_count != 0u && local_candidates == NULL))
        return false;
    *plan                       = {};
    plan->family                = family;
    plan->local_port            = local_port;
    plan->local_candidates      = local_candidates;
    plan->local_candidate_count = local_candidate_count;
    if (reported_public_endpoint != NULL &&
        (reported_public_endpoint->family != family || reported_public_endpoint->port == 0u))
        return false;
    if (reported_public_endpoint != NULL && !append_public_candidate(plan, *reported_public_endpoint)) return false;
    if (plan->public_candidate_count < public_candidate_count &&
        !append_public_candidate(plan, observed_public_endpoint))
        return false;
    plan->public_candidates = plan->decoded_public_candidates;
    return plan->public_candidate_count != 0u;
}

bool Registration::matchesToken(const uint8_t value[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE]) const
{
    return memcmp(value, token.data(), token.size()) == 0;
}

bool Registration::acceptsFamily(const utp_ntrs_endpoint_t& observed) const
{
    return (local_family == UTP_ADDRESS_FAMILY_IPV4 && observed.family == AF_INET) ||
           (local_family == UTP_ADDRESS_FAMILY_IPV6 && observed.family == AF_INET6);
}

void Registration::updateEndpoint(const utp_ntrs_endpoint_t& observed)
{
    if (!endpoint_ips_equal(endpoint, observed)) {
        observed_addresses     = {};
        observed_at_unix_ms    = {};
        observed_address_count = 0u;
    }
    endpoint = observed;
}

void Registration::touch(uint64_t now_ms)
{
    last_activity_ms        = now_ms;
    keepalive_packet_number = 0u;
    last_keepalive_ping_ms  = 0u;
}

void Registration::update(const utp_rendezvous_register_t& request, const utp_ntrs_endpoint_t& observed)
{
    updateEndpoint(observed);
    nat_class                    = request.nat_class;
    local_family                 = request.local_family;
    local_port                   = request.local_port;
    local_candidate_count        = request.local_candidate_count;
    has_reported_public_endpoint = request.reported_public_endpoint != NULL;
    if (has_reported_public_endpoint) reported_public_endpoint = *request.reported_public_endpoint;
    for (uint8_t index = 0u; index < request.local_candidate_count; ++index)
        local_candidates[index] = request.local_candidates[index];
    touch(utp_ntrs_now_ms());
}

uint8_t Registration::updateObservedAddresses(const utp_rendezvous_address_update_t& update)
{
    uint8_t accepted = 0u;

    for (uint8_t index = 0u; index < update.sample_count; ++index) {
        const utp_address_t& sample      = update.samples[index];
        uint8_t              replacement = 0u;

        if (!address_matches_endpoint_ip(sample, endpoint)) continue;
        for (uint8_t existing = 0u; existing < observed_address_count; ++existing) {
            if (!addresses_equal(sample, observed_addresses[existing])) continue;
            if (update.observed_at_unix_ms[index] >= observed_at_unix_ms[existing]) {
                observed_at_unix_ms[existing] = update.observed_at_unix_ms[index];
                ++accepted;
            }
            replacement = UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES;
            break;
        }
        if (replacement == UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES) continue;
        if (observed_address_count < UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES) {
            replacement = observed_address_count++;
        } else {
            for (uint8_t existing = 1u; existing < UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES; ++existing) {
                if (observed_at_unix_ms[existing] < observed_at_unix_ms[replacement]) replacement = existing;
            }
            if (update.observed_at_unix_ms[index] <= observed_at_unix_ms[replacement]) continue;
        }
        observed_addresses[replacement]  = sample;
        observed_at_unix_ms[replacement] = update.observed_at_unix_ms[index];
        ++accepted;
    }
    return accepted;
}

void Registration::recordCalibrationEndpoint(uint8_t index, const utp_ntrs_endpoint_t& observed)
{
    if (index >= calibration_endpoint_count || observed.port == 0u) return;
    if (!endpoint_ips_equal(calibration_ip, observed)) {
        calibration_ip_consistent = false;
        return;
    }
    calibration_ports[index + 1u]  = observed.port;
    calibration_received_mask     |= static_cast<uint8_t>(UINT8_C(1) << (index + 1u));
}

bool NtrsServer::beginCalibration(Registration* registration)
{
    assert(registration != NULL);
    registration->calibration_id             = 0u;
    registration->calibration_ip             = registration->endpoint;
    registration->calibration_endpoint_count = calibration_endpoint_count_;
    registration->calibration_ports          = {};
    registration->calibration_ip_consistent  = true;
    registration->calibration_ports[0u]      = registration->endpoint.port;
    registration->calibration_received_mask  = UINT8_C(0x01);
    if (calibration_endpoint_count_ == 0u) return true;
    return random_u64(&registration->calibration_id);
}

static bool appendRegistrationObservedCandidates(utp_rendezvous_candidate_plan_t* plan,
                                                 const Registration& registration, uint8_t public_candidate_count)
{
    std::array<bool, UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES> selected = {};

    for (uint8_t count = 0u; count < registration.observed_address_count; ++count) {
        uint8_t  newest    = UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES;
        uint64_t newest_at = 0u;

        for (uint8_t index = 0u; index < registration.observed_address_count; ++index) {
            if (!selected[index] &&
                (newest == UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES || registration.observed_at_unix_ms[index] > newest_at)) {
                newest    = index;
                newest_at = registration.observed_at_unix_ms[index];
            }
        }
        if (newest == UTP_RENDEZVOUS_MAX_ADDRESS_SAMPLES) break;
        selected[newest] = true;
        if (plan->public_candidate_count >= public_candidate_count) break;
        if (!append_public_candidate(plan, registration.observed_addresses[newest])) return false;
    }
    return true;
}

static uint16_t advance_port(uint16_t port, int delta)
{
    int value = static_cast<int>(port) - 1 + delta;

    value %= 65535;
    if (value < 0) value += 65535;
    return static_cast<uint16_t>(value + 1);
}

static bool appendRegistrationPredictedCandidates(utp_rendezvous_candidate_plan_t* plan,
                                                  const Registration& registration, uint8_t public_candidate_count)
{
    utp_address_t candidate                = {};
    bool          predictable              = false;
    int           step                     = 0;
    uint8_t       expected_mask            = 0u;
    uint8_t       calibration_sample_count = 0u;

    if (registration.nat_class != UTP_NAT_CLASS_SYMMETRIC && registration.nat_class != UTP_NAT_CLASS_UNKNOWN)
        return true;
    if (!endpoint_to_address(registration.endpoint, &candidate)) return false;
    calibration_sample_count = (uint8_t)(registration.calibration_endpoint_count + 1u);
    if (calibration_sample_count >= 3u) {
        expected_mask = static_cast<uint8_t>((UINT8_C(1) << calibration_sample_count) - 1u);
        if (registration.calibration_ip_consistent && registration.calibration_received_mask == expected_mask) {
            predictable = true;
            for (uint8_t index = 1u; index < calibration_sample_count; ++index) {
                int current_step = static_cast<int>(registration.calibration_ports[index]) -
                                   static_cast<int>(registration.calibration_ports[index - 1u]);

                if (current_step > 32767)
                    current_step -= 65535;
                else if (current_step < -32767)
                    current_step += 65535;
                if (current_step == 0 || current_step < -1024 || current_step > 1024 ||
                    (index > 1u && current_step != step)) {
                    predictable = false;
                    break;
                }
                step = current_step;
            }
        }
    }
    if (predictable) {
        candidate.port = registration.calibration_ports[calibration_sample_count - 1u];
        while (plan->public_candidate_count < public_candidate_count) {
            candidate.port = advance_port(candidate.port, step);
            if (!append_public_candidate(plan, candidate)) return false;
        }
    }
    while (plan->public_candidate_count < public_candidate_count) {
        if (!random_port(&candidate.port) || !append_public_candidate(plan, candidate)) return false;
    }
    return true;
}

static bool appendTemporaryPredictedCandidates(utp_rendezvous_candidate_plan_t* plan,
                                               const PendingRendezvous& transaction, uint8_t public_candidate_count)
{
    utp_address_t candidate = {};
    int           first_step;
    int           second_step;

    if (!endpoint_to_address(transaction.calibration_ip, &candidate)) return false;
    first_step =
        static_cast<int>(transaction.calibration_ports[1u]) - static_cast<int>(transaction.calibration_ports[0u]);
    second_step =
        static_cast<int>(transaction.calibration_ports[2u]) - static_cast<int>(transaction.calibration_ports[1u]);
    if (first_step > 32767)
        first_step -= 65535;
    else if (first_step < -32767)
        first_step += 65535;
    if (second_step > 32767)
        second_step -= 65535;
    else if (second_step < -32767)
        second_step += 65535;
    if (!transaction.calibration_ip_consistent || transaction.calibration_received_mask != UINT8_C(0x07) ||
        first_step == 0 || first_step != second_step || first_step < -1024 || first_step > 1024) {
        while (plan->public_candidate_count < public_candidate_count) {
            if (!random_port(&candidate.port) || !append_public_candidate(plan, candidate)) return false;
        }
        return true;
    }
    candidate.port = transaction.calibration_ports[2u];
    while (plan->public_candidate_count < public_candidate_count) {
        candidate.port = advance_port(candidate.port, first_step);
        if (!append_public_candidate(plan, candidate)) return false;
    }
    return true;
}

static std::string make_endpoint_text(const std::string& address, uint16_t port)
{
    const std::string port_text = std::to_string(port);

    if (!address.empty() && address[0] == '[') return address + ":" + port_text;
    if (address.find(':') != std::string::npos) return "[" + address + "]:" + port_text;
    return address + ":" + port_text;
}

struct OutgoingFrame {
    const uint8_t* body;
    size_t         body_length;
    uint8_t        message_type;
};

bool NtrsServer::sendDatagram(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                              const uint8_t* packet, size_t packet_length)
{
    assert(socket != NULL);
    assert(shared_state_ != NULL);
    assert(packet != NULL);
    if (socket->fd < 0 || fatal_socket_error_ || shared_state_->fatal_socket_error.load(std::memory_order_relaxed) ||
        packet_length == 0u || packet_length > UTP_PACKET_MTU_FLOOR)
        return false;
    PreparedDatagram datagram = {};

    datagram.socket        = socket;
    datagram.peer          = peer;
    datagram.peer_length   = peer_length;
    datagram.packet_length = packet_length;
    memcpy(datagram.packet.data(), packet, packet_length);
    return enqueueControlOutput(datagram);
}

bool NtrsServer::sendDatagram(const PreparedDatagram& datagram)
{
    return sendDatagram(datagram.socket, datagram.peer, datagram.peer_length, datagram.packet.data(),
                        datagram.packet_length);
}

bool NtrsServer::enqueueControl(const ControlTask& task)
{
    const uint8_t           wakeup = 1u;
    MpscQueue<ControlTask>* queue  = NULL;

    assert(shared_state_ != NULL);
    assert(shared_state_->control_notify_write_fd >= 0);
    queue = &shared_state_->lifecycle_control_queue;
    if (task.message_type == UTP_RENDEZVOUS_MESSAGE_PING || task.message_type == UTP_RENDEZVOUS_MESSAGE_PONG)
        queue = &shared_state_->keepalive_control_queue;
    if (!queue->push(task)) return false;

    while (write(shared_state_->control_notify_write_fd, &wakeup, sizeof(wakeup)) < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            stopForFatalError(errno, "control wakeup");
            return false;
        }
        break;
    }
    return true;
}

bool NtrsServer::enqueueCalibrationStart(const CalibrationStartTask& task)
{
    const uint8_t wakeup = 1u;

    assert(shared_state_ != NULL);
    assert(shared_state_->control_notify_write_fd >= 0);
    if (!shared_state_->calibration_start_queue.push(task)) return false;
    while (write(shared_state_->control_notify_write_fd, &wakeup, sizeof(wakeup)) < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            stopForFatalError(errno, "control wakeup");
            return false;
        }
        break;
    }
    return true;
}

bool NtrsServer::enqueueControlOutput(const PreparedDatagram& datagram)
{
    OutgoingDatagram output = {};
    const uint8_t    wakeup = 1u;

    assert(datagram.socket != NULL);
    assert(datagram.socket->control_output_write_fd >= 0);
    output.peer          = datagram.peer;
    output.peer_length   = datagram.peer_length;
    output.packet        = datagram.packet;
    output.packet_length = datagram.packet_length;
    size_t expected      = datagram.socket->control_output_queue_size.load(std::memory_order_relaxed);
    do {
        if (expected >= output_queue_capacity_) return false;
    } while (!datagram.socket->control_output_queue_size.compare_exchange_weak(
        expected, expected + 1u, std::memory_order_relaxed, std::memory_order_relaxed));
    if (!datagram.socket->control_output_queue.push(output)) {
        datagram.socket->control_output_queue_size.fetch_sub(1u, std::memory_order_relaxed);
        return false;
    }
    while (write(datagram.socket->control_output_write_fd, &wakeup, sizeof(wakeup)) < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            stopForFatalError(errno, "output wakeup");
            return false;
        }
        break;
    }
    return true;
}

static size_t rendezvous_hash(const uint8_t* id, size_t length, size_t worker_count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t   index;

    assert(id != NULL);
    assert(length != 0u);
    assert(worker_count != 0u);
    for (index = 0u; index < length; ++index) {
        hash ^= id[index];
        hash *= UINT64_C(1099511628211);
    }
    return static_cast<size_t>(hash % worker_count);
}

static std::string registration_token_key(const uint8_t token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE])
{
    return std::string(reinterpret_cast<const char*>(token), UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE);
}

static std::string rendezvous_route_key(const uint8_t token[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE],
                                        uint64_t      packet_number)
{
    std::string key = registration_token_key(token);

    for (size_t index = 0u; index < sizeof(packet_number); ++index) {
        const size_t shift = (sizeof(packet_number) - index - 1u) * 8u;
        key.push_back(static_cast<char>((packet_number >> shift) & UINT64_C(0xff)));
    }
    return key;
}

bool NtrsServer::isRegistrationTokenActive(
    const std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE>& token) const
{
    assert(shared_state_ != NULL);
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    return shared_state_->active_registration_tokens.find(registration_token_key(token.data())) !=
           shared_state_->active_registration_tokens.end();
}

void NtrsServer::erasePendingRendezvous(const std::string& rendezvous_key)
{
    std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.find(rendezvous_key);

    if (entry == pending_rendezvous_.end()) return;
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    if (entry->second.forward_packet_number != 0u)
        shared_state_->forward_owners.erase(
            rendezvous_route_key(entry->second.target_token.data(), entry->second.forward_packet_number));
    pending_rendezvous_.erase(entry);
}

bool NtrsServer::enqueueWorker(const WorkerTask& task)
{
    const uint8_t wakeup = 1u;

    assert(worker_notify_write_fd_ >= 0);
    if (!worker_queue_.push(task)) return false;
    while (write(worker_notify_write_fd_, &wakeup, sizeof(wakeup)) < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            stopForFatalError(errno, "worker wakeup");
            return false;
        }
        break;
    }
    return true;
}

bool NtrsServer::encodeFrames(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                              const OutgoingFrame* frames, size_t frame_count, uint64_t* packet_number,
                              PreparedDatagram* encoded_datagram)
{
    uint8_t             packet[UTP_PACKET_MTU_FLOOR] = {};
    utp_packet_header_t header                       = {};
    size_t              payload_length               = 0u;
    size_t              offset                       = UTP_PACKET_HEADER_SIZE;

    assert(socket != NULL);
    assert(frames != NULL);
    assert(frame_count != 0u);
    assert(shared_state_ != NULL);
    assert(encoded_datagram != NULL);
    for (size_t index = 0u; index < frame_count; ++index) {
        if (frames[index].message_type == 0u || frames[index].body_length > UINT16_MAX ||
            (frames[index].body == NULL && frames[index].body_length != 0u) ||
            payload_length > UINT16_MAX - UTP_FRAME_RENDEZVOUS_HEADER_SIZE - frames[index].body_length)
            return false;
        payload_length += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body_length;
    }
    if (UTP_PACKET_HEADER_SIZE + payload_length > sizeof(packet)) return false;
    header.packet_number = shared_state_->nextPacketNumber();
    if (header.packet_number == 0u) return false;
    header.payload_length = static_cast<uint16_t>(payload_length);
    header.type           = UTP_PACKET_TYPE_RENDEZVOUS;
    if (utp_proto_encode_header(packet, sizeof(packet), &header) != UTP_INTERNAL_ERROR_OK) return false;
    for (size_t index = 0u; index < frame_count; ++index) {
        const utp_frame_rendezvous_t frame = {frames[index].body, static_cast<uint16_t>(frames[index].body_length),
                                              frames[index].message_type};
        if (utp_frame_rendezvous_encode(packet + offset, sizeof(packet) - offset, &frame) != UTP_INTERNAL_ERROR_OK)
            return false;
        offset += UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frames[index].body_length;
    }
    encoded_datagram->socket        = socket;
    encoded_datagram->peer          = peer;
    encoded_datagram->peer_length   = peer_length;
    encoded_datagram->packet_length = offset;
    memcpy(encoded_datagram->packet.data(), packet, offset);
    if (packet_number != NULL) *packet_number = header.packet_number;
    return true;
}

bool NtrsServer::sendFrames(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                            const OutgoingFrame* frames, size_t frame_count, uint64_t* packet_number,
                            std::array<uint8_t, UTP_PACKET_MTU_FLOOR>* encoded_packet, size_t* encoded_packet_length)
{
    PreparedDatagram datagram = {};

    if (!encodeFrames(socket, peer, peer_length, frames, frame_count, packet_number, &datagram)) return false;
    if (encoded_packet != NULL && encoded_packet_length != NULL) {
        *encoded_packet        = datagram.packet;
        *encoded_packet_length = datagram.packet_length;
    }
    return sendDatagram(datagram);
}

bool NtrsServer::sendMessage(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                             uint8_t message_type, const uint8_t* body, size_t body_length)
{
    const OutgoingFrame frame = {body, body_length, message_type};

    return sendFrames(socket, peer, peer_length, &frame, 1u, NULL);
}

bool NtrsServer::sendRejected(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                              uint8_t rejected_message_type, const uint8_t* reference_id, uint8_t reference_length,
                              uint16_t reason_code)
{
    utp_rendezvous_rejected_t rejected                          = {};
    uint8_t                   body[UTP_RENDEZVOUS_ID_SIZE + 4u] = {};
    size_t                    body_length                       = 0u;

    assert(reference_id != NULL);
    assert(reference_length <= sizeof(rejected.reference_id));
    rejected.rejected_message_type = rejected_message_type;
    rejected.reference_length      = reference_length;
    rejected.reason_code           = reason_code;
    memcpy(rejected.reference_id, reference_id, reference_length);
    if (utp_rendezvous_rejected_encode(body, sizeof(body), &rejected, &body_length) != UTP_INTERNAL_ERROR_OK)
        return false;
    return sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REJECTED, body, body_length);
}

bool NtrsServer::prepareKeepalivePing(const Registration& registration, PreparedDatagram* datagram,
                                      uint64_t* packet_number)
{
    utp_rendezvous_ping_t ping                                                            = {};
    uint8_t               body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};
    sockaddr_storage      peer                                                            = {};
    socklen_t             peer_length                                                     = 0u;
    OutgoingFrame         frame;

    assert(datagram != NULL);
    if (!utp_ntrs_endpoint_to_sockaddr(&registration.endpoint, &peer, &peer_length)) return false;
    memcpy(ping.registration_token, registration.token.data(), sizeof(ping.registration_token));
    if (utp_rendezvous_ping_encode(body, sizeof(body), &ping) != UTP_INTERNAL_ERROR_OK) return false;
    frame = OutgoingFrame{body, sizeof(body), UTP_RENDEZVOUS_MESSAGE_PING};
    return encodeFrames(&primary_socket_, peer, peer_length, &frame, 1u, packet_number, datagram);
}

bool NtrsServer::prepareCalibration(const PendingRendezvous& transaction, PreparedDatagram* datagram)
{
    utp_rendezvous_calibrate_t calibrate                  = {};
    uint8_t                    body[UTP_PACKET_MTU_FLOOR] = {};
    size_t                     body_length                = 0u;

    assert(datagram != NULL);
    if (!transaction.calibration_pending || calibration_endpoint_count_ < 2u || transaction.calibration_id == 0u) {
        return false;
    }
    memcpy(calibrate.rendezvous_id, transaction.rendezvous_id.data(), sizeof(calibrate.rendezvous_id));
    memcpy(calibrate.calibration_token, transaction.calibration_token.data(), sizeof(calibrate.calibration_token));
    calibrate.calibration_id = transaction.calibration_id;
    calibrate.endpoint_count = k_temporary_calibration_endpoint_count;
    calibrate.endpoints      = calibration_endpoints_.data();
    if (utp_rendezvous_calibrate_encode(body, sizeof(body), &calibrate, &body_length) != UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    const OutgoingFrame frame = {body, body_length, UTP_RENDEZVOUS_MESSAGE_CALIBRATE};
    return encodeFrames(&primary_socket_, transaction.source_socket_address, transaction.source_socket_length, &frame,
                        1u, NULL, datagram);
}

bool NtrsServer::preparePendingRendezvous(PendingRendezvous* transaction, PreparedDatagram* redirect_datagram,
                                          PreparedDatagram* forward_datagram)
{
    utp_rendezvous_candidate_plan_t source_plan                                                          = {};
    utp_rendezvous_forward_t        forward                                                              = {};
    utp_rendezvous_ping_t           ping                                                                 = {};
    uint8_t                         forward_body[UTP_PACKET_MTU_FLOOR]                                   = {};
    uint8_t                         ping_body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};
    size_t                          forward_body_length                                                  = 0u;

    assert(transaction != NULL);
    assert(redirect_datagram != NULL);
    assert(forward_datagram != NULL);
    if (transaction->source_peer_id_length == 0u ||
        !make_candidate_plan(
            &source_plan, transaction->source_local_family, transaction->source_local_port,
            transaction->source_local_candidates.data(), transaction->source_local_candidate_count,
            transaction->has_source_reported_public_endpoint ? &transaction->source_reported_public_endpoint : NULL,
            transaction->source_endpoint, public_candidate_count_)) {
        return false;
    }
    if (transaction->calibration_id != 0u &&
        !appendTemporaryPredictedCandidates(&source_plan, *transaction, public_candidate_count_)) {
        return false;
    }
    forward.source_peer_id        = transaction->source_peer_id.data();
    forward.source_peer_id_length = transaction->source_peer_id_length;
    memcpy(forward.rendezvous_id, transaction->rendezvous_id.data(), sizeof(forward.rendezvous_id));
    memcpy(forward.punch_token, transaction->punch_token.data(), sizeof(forward.punch_token));
    forward.source_plan = source_plan;
    if (utp_rendezvous_forward_encode(forward_body, sizeof(forward_body), &forward, &forward_body_length) !=
        UTP_INTERNAL_ERROR_OK) {
        return false;
    }
    memcpy(ping.registration_token, transaction->target_token.data(), sizeof(ping.registration_token));
    if (utp_rendezvous_ping_encode(ping_body, sizeof(ping_body), &ping) != UTP_INTERNAL_ERROR_OK) return false;
    transaction->calibration_pending       = false;
    transaction->forward_retry_at_ms       = utp_ntrs_now_ms() + k_forward_retry_initial_delay_ms;
    transaction->forward_retries_remaining = k_forward_retry_count;
    transaction->forward_retry_delay_ms    = k_forward_retry_initial_delay_ms;
    const OutgoingFrame frames[2]          = {{ping_body, sizeof(ping_body), UTP_RENDEZVOUS_MESSAGE_PING},
                                              {forward_body, forward_body_length, UTP_RENDEZVOUS_MESSAGE_FORWARD}};
    const OutgoingFrame redirect_frame     = {transaction->redirect_body.data(), transaction->redirect_body_length,
                                              UTP_RENDEZVOUS_MESSAGE_REDIRECT};
    if (!encodeFrames(&primary_socket_, transaction->source_socket_address, transaction->source_socket_length,
                      &redirect_frame, 1u, NULL, redirect_datagram)) {
        return false;
    }
    if (!encodeFrames(&primary_socket_, transaction->target_socket_address, transaction->target_socket_length, frames,
                      2u, &transaction->forward_packet_number, forward_datagram)) {
        return false;
    }
    transaction->forward_packet        = forward_datagram->packet;
    transaction->forward_packet_length = forward_datagram->packet_length;
    return true;
}

void NtrsServer::expireRegistrations(uint64_t now_ms)
{
    for (std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.begin();
         entry != shared_state_->registrations.end();) {
        if (now_ms - entry->second.last_activity_ms >= registration_timeout_ms_) {
            const std::array<uint8_t, UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE> token = entry->second.token;
            shared_state_->active_registration_tokens.erase(registration_token_key(token.data()));
            entry = shared_state_->registrations.erase(entry);
        } else
            ++entry;
    }
    for (std::unordered_map<std::string, uint64_t>::iterator entry = shared_state_->unregistration_tombstones.begin();
         entry != shared_state_->unregistration_tombstones.end();) {
        if (entry->second <= now_ms)
            entry = shared_state_->unregistration_tombstones.erase(entry);
        else
            ++entry;
    }
}

void NtrsServer::handleRegister(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                                const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_register_t request                              = {};
    uint8_t                   body[UTP_PACKET_MTU_FLOOR]           = {};
    char                      observed_text[INET6_ADDRSTRLEN + 8u] = {};
    size_t                    body_length                          = 0u;
    bool                      initial_retry                        = false;
    bool                      registration_created                 = false;
    bool                      begin_calibration                    = false;
    bool                      rejected                             = false;
    uint16_t                  rejection_reason                     = 0u;

    assert(shared_state_ != NULL);
    if (utp_rendezvous_register_decode(&request, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV4 && observed.family != AF_INET) ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV6 && observed.family != AF_INET6))
        return;
    const std::string           peer_id(reinterpret_cast<const char*>(request.peer_id), request.peer_id_length);
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    {
        std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.find(peer_id);
        if (entry == shared_state_->registrations.end()) {
            if (!token_is_zero(request.registration_token)) {
                rejected         = true;
                rejection_reason = k_rejection_token_invalid;
            } else {
                Registration registration = {};
                if (!random_token(&registration.token)) return;
                registration.last_register_request_id  = request.registration_request_id;
                registration.last_register_was_initial = true;
                entry                = shared_state_->registrations.insert(std::make_pair(peer_id, registration)).first;
                registration_created = true;
            }
        } else {
            initial_retry = token_is_zero(request.registration_token) && entry->second.last_register_was_initial &&
                            request.registration_request_id == entry->second.last_register_request_id;
            const bool update =
                !token_is_zero(request.registration_token) && entry->second.matchesToken(request.registration_token);

            if (!initial_retry && !update) {
                rejected = true;
                rejection_reason =
                    token_is_zero(request.registration_token) ? k_rejection_peer_id_exists : k_rejection_token_invalid;
            } else if (update) {
                entry->second.last_register_request_id  = request.registration_request_id;
                entry->second.last_register_was_initial = false;
            }
        }
        if (!rejected) {
            begin_calibration = request.nat_class == UTP_NAT_CLASS_SYMMETRIC &&
                                (registration_created || entry->second.calibration_id == 0u);
            entry->second.update(request, observed);
            if (begin_calibration && !beginCalibration(&entry->second)) return;
            if (request.nat_class != UTP_NAT_CLASS_SYMMETRIC) {
                entry->second.calibration_id             = 0u;
                entry->second.calibration_ip             = {};
                entry->second.calibration_endpoint_count = 0u;
                entry->second.calibration_received_mask  = 0u;
                entry->second.calibration_ports          = {};
                entry->second.calibration_ip_consistent  = false;
            }
            utp_rendezvous_registered_t reply = {};
            reply.registration_request_id     = request.registration_request_id;
            memcpy(reply.registration_token, entry->second.token.data(), entry->second.token.size());
            if (request.nat_class == UTP_NAT_CLASS_SYMMETRIC && (begin_calibration || initial_retry)) {
                reply.calibration_id             = entry->second.calibration_id;
                reply.calibration_endpoint_count = entry->second.calibration_endpoint_count;
                reply.calibration_endpoints      = calibration_endpoints_.data();
            }
            if (utp_rendezvous_registered_encode(body, sizeof(body), &reply, &body_length) != UTP_INTERNAL_ERROR_OK)
                return;
            shared_state_->active_registration_tokens.insert(registration_token_key(entry->second.token.data()));
        }
    }
    if (rejected)
        (void)sendRejected(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REGISTER, frame.payload,
                           UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE, rejection_reason);
    else
        (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REGISTERED, body, body_length);
    fprintf(stderr, "NTRS <- Node=%s [Register] peer_id=%s\n",
            utp_ntrs_endpoint_format(&observed, observed_text, sizeof(observed_text)), peer_id.c_str());
}

void NtrsServer::handlePing(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                            const utp_ntrs_endpoint_t& observed, const utp_packet_header_t& header,
                            const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_ping_t ping                                                            = {};
    utp_rendezvous_pong_t pong                                                            = {};
    uint8_t               body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};
    bool                  send_pong                                                       = false;

    assert(shared_state_ != NULL);
    assert(socket != NULL);
    if (utp_rendezvous_ping_decode(&ping, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK)
        return;
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    {
        for (std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.begin();
             entry != shared_state_->registrations.end(); ++entry) {
            if (!entry->second.matchesToken(ping.registration_token)) continue;
            if (!entry->second.acceptsFamily(observed)) return;
            if (socket->calibration_index == k_primary_socket_index) {
                if (ping.calibration_id != 0u) return;
                entry->second.updateEndpoint(observed);
            } else {
                if (ping.calibration_id == 0u || ping.calibration_id != entry->second.calibration_id ||
                    socket->calibration_index >= entry->second.calibration_endpoint_count)
                    return;
                entry->second.recordCalibrationEndpoint(socket->calibration_index, observed);
            }
            entry->second.touch(utp_ntrs_now_ms());
            send_pong = true;
            break;
        }
    }
    if (!send_pong) return;
    memcpy(pong.registration_token, ping.registration_token, sizeof(pong.registration_token));
    pong.acknowledged_packet_number = header.packet_number;
    if (utp_rendezvous_pong_encode(body, sizeof(body), &pong) == UTP_INTERNAL_ERROR_OK)
        (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_PONG, body, sizeof(body));
}

bool NtrsServer::enqueueCalibrationResult(const CalibrationSession& session)
{
    WorkerTask task = {};

    assert(shared_state_ != NULL);
    if (session.owner_worker >= shared_state_->workers.size()) return false;
    task.task_type                 = kWorkerTaskCalibrationAccepted;
    task.rendezvous_id             = session.rendezvous_id;
    task.calibration_token         = session.calibration_token;
    task.calibration_ip            = session.calibration_ip;
    task.calibration_ports         = session.calibration_ports;
    task.calibration_id            = session.calibration_id;
    task.calibration_received_mask = session.received_mask;
    task.calibration_ip_consistent = session.ip_consistent;
    task.calibration_complete      = session.completed;
    return shared_state_->workers[session.owner_worker]->enqueueWorker(task);
}

void NtrsServer::handleCalibrationStart(const CalibrationStartTask& task)
{
    const std::string key = rendezvous_route_key(task.calibration_token.data(), task.calibration_id);
    std::unordered_map<std::string, CalibrationSession>::iterator entry = calibration_sessions_.find(key);

    if (entry == calibration_sessions_.end()) {
        CalibrationSession session = {};

        session.rendezvous_id     = task.rendezvous_id;
        session.calibration_token = task.calibration_token;
        session.calibration_ip   = task.observed;
        session.calibration_ports[0u] = task.observed.port;
        session.calibration_id        = task.calibration_id;
        session.deadline_ms           = task.deadline_ms;
        session.owner_worker          = task.owner_worker;
        session.received_mask         = UINT8_C(0x01);
        session.ip_consistent         = true;
        entry                        = calibration_sessions_.insert(std::make_pair(key, session)).first;
    }
    if (entry->second.completed) {
        if (!entry->second.result_delivered && enqueueCalibrationResult(entry->second))
            entry->second.result_delivered = true;
    } else if (!entry->second.ready_delivered && enqueueCalibrationResult(entry->second)) {
        entry->second.ready_delivered = true;
    }
}

void NtrsServer::handleCalibrationPing(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                                       const utp_ntrs_endpoint_t& observed, const utp_packet_header_t& header,
                                       const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_ping_t ping                                                            = {};
    utp_rendezvous_pong_t pong                                                            = {};
    uint8_t               body[UTP_RENDEZVOUS_REGISTRATION_TOKEN_SIZE + sizeof(uint64_t)] = {};
    const uint64_t        now_ms                                                           = utp_ntrs_now_ms();
    CalibrationSession*   session;

    assert(socket != NULL);
    if (utp_rendezvous_ping_decode(&ping, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK ||
        ping.calibration_id == 0u || socket->calibration_index >= k_temporary_calibration_endpoint_count)
        return;
    const std::string key = rendezvous_route_key(ping.registration_token, ping.calibration_id);
    std::unordered_map<std::string, CalibrationSession>::iterator entry = calibration_sessions_.find(key);
    if (entry == calibration_sessions_.end()) return;
    session = &entry->second;
    if (!session->completed) {
        const uint8_t index = static_cast<uint8_t>(socket->calibration_index + 1u);

        if (!endpoint_ips_equal(session->calibration_ip, observed)) {
            session->ip_consistent = false;
        } else if (observed.port != 0u) {
            session->calibration_ports[index] = observed.port;
            session->received_mask |= static_cast<uint8_t>(UINT8_C(1) << index);
            if (session->received_mask == UINT8_C(0x07)) {
                session->completed       = true;
                session->completed_at_ms = now_ms;
            }
        }
    }
    memcpy(pong.registration_token, ping.registration_token, sizeof(pong.registration_token));
    pong.acknowledged_packet_number = header.packet_number;
    if (utp_rendezvous_pong_encode(body, sizeof(body), &pong) == UTP_INTERNAL_ERROR_OK)
        (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_PONG, body, sizeof(body));
    if (session->completed && !session->result_delivered && enqueueCalibrationResult(*session))
        session->result_delivered = true;
}

void NtrsServer::handleCalibrationAccepted(const WorkerTask& task)
{
    const std::string key(reinterpret_cast<const char*>(task.rendezvous_id.data()), task.rendezvous_id.size());
    std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.find(key);
    PendingRendezvous* transaction;
    PreparedDatagram  redirect = {};
    PreparedDatagram  forward  = {};

    if (entry == pending_rendezvous_.end()) return;
    transaction = &entry->second;
    if (!transaction->calibration_pending || transaction->calibration_id != task.calibration_id ||
        transaction->calibration_token != task.calibration_token)
        return;
    if (!task.calibration_complete) {
        if (prepareCalibration(*transaction, &redirect)) (void)sendDatagram(redirect);
        return;
    }
    transaction->calibration_ip             = task.calibration_ip;
    transaction->calibration_ports          = task.calibration_ports;
    transaction->calibration_received_mask  = task.calibration_received_mask;
    transaction->calibration_ip_consistent  = task.calibration_ip_consistent;
    if (!isRegistrationTokenActive(transaction->target_token)) {
        erasePendingRendezvous(key);
        return;
    }
    if (!preparePendingRendezvous(transaction, &redirect, &forward)) {
        transaction->calibration_pending = true;
        return;
    }
    transaction->calibration_pending = false;
    {
        std::lock_guard<std::mutex> lock(shared_state_->mutex);
        shared_state_->forward_owners[
            rendezvous_route_key(transaction->target_token.data(), transaction->forward_packet_number)] = worker_index_;
    }
    (void)sendDatagram(redirect);
    (void)sendDatagram(forward);
}

void NtrsServer::handlePong(const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_pong_t pong = {};

    assert(shared_state_ != NULL);
    if (utp_rendezvous_pong_decode(&pong, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK)
        return;
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    for (std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.begin();
         entry != shared_state_->registrations.end(); ++entry) {
        Registration& registration = entry->second;

        if (endpoints_equal(registration.endpoint, observed) && registration.matchesToken(pong.registration_token)) {
            if (registration.keepalive_packet_number == pong.acknowledged_packet_number) {
                registration.keepalive_packet_number = 0u;
                registration.last_keepalive_ping_ms  = 0u;
            }
            registration.touch(utp_ntrs_now_ms());
            return;
        }
    }
}

void NtrsServer::handleForwardPong(const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_pong_t pong = {};

    assert(shared_state_ != NULL);
    if (utp_rendezvous_pong_decode(&pong, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK)
        return;
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end(); ++entry) {
        PendingRendezvous& transaction = entry->second;
        if (!endpoints_equal(transaction.target_endpoint, observed) ||
            memcmp(transaction.target_token.data(), pong.registration_token, transaction.target_token.size()) != 0 ||
            transaction.forward_packet_number != pong.acknowledged_packet_number)
            continue;
        if (!isRegistrationTokenActive(transaction.target_token)) {
            erasePendingRendezvous(entry->first);
            return;
        }
        transaction.forward_delivered = true;
        {
            std::lock_guard<std::mutex> lock(shared_state_->mutex);
            shared_state_->forward_owners.erase(
                rendezvous_route_key(pong.registration_token, pong.acknowledged_packet_number));
        }
        ControlTask task = {};

        task.observed             = observed;
        task.message_type         = UTP_RENDEZVOUS_MESSAGE_PONG;
        task.frame_payload_length = frame.payload_length;
        memcpy(task.frame_payload.data(), frame.payload, frame.payload_length);
        (void)enqueueControl(task);
        return;
    }
}

void NtrsServer::handleAddressUpdate(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                                     const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_address_update_t  update                               = {};
    utp_rendezvous_address_updated_t updated                              = {};
    uint8_t                          body[sizeof(updated.update_id)]      = {};
    uint8_t                          accepted                             = 0u;
    size_t                           body_length                          = 0u;
    char                             observed_text[INET6_ADDRSTRLEN + 8u] = {};

    assert(shared_state_ != NULL);
    if (utp_rendezvous_address_update_decode(&update, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK)
        return;
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    {
        for (std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.begin();
             entry != shared_state_->registrations.end(); ++entry) {
            if (!entry->second.matchesToken(update.registration_token)) continue;
            if (!endpoints_equal(entry->second.endpoint, observed)) return;
            accepted = entry->second.updateObservedAddresses(update);
            entry->second.touch(utp_ntrs_now_ms());
            updated.update_id = update.update_id;
            if (utp_rendezvous_address_updated_encode(body, sizeof(body), &updated) != UTP_INTERNAL_ERROR_OK) return;
            body_length = sizeof(body);
            break;
        }
    }
    if (body_length == 0u) return;
    (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_ADDRESS_UPDATED, body, body_length);
    fprintf(stderr, "NTRS <- Node=%s [AddressUpdate] samples=%u accepted=%u\n",
            utp_ntrs_endpoint_format(&observed, observed_text, sizeof(observed_text)),
            static_cast<unsigned>(update.sample_count), static_cast<unsigned>(accepted));
}

void NtrsServer::handleUnregister(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                                  const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_unregister_t unregister_message = {};
    const uint64_t              now_ms             = utp_ntrs_now_ms();

    assert(shared_state_ != NULL);
    if (utp_rendezvous_unregister_decode(&unregister_message, frame.payload,
                                         frame.payload_length) != UTP_INTERNAL_ERROR_OK)
        return;
    const std::string key          = registration_token_key(unregister_message.registration_token);
    bool              unregistered = false;

    {
        std::lock_guard<std::mutex> lock(shared_state_->mutex);
        for (std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.begin();
             entry != shared_state_->registrations.end(); ++entry) {
            if (entry->second.matchesToken(unregister_message.registration_token)) {
                shared_state_->active_registration_tokens.erase(key);
                shared_state_->registrations.erase(entry);
                shared_state_->unregistration_tombstones[key] = now_ms + k_unregistration_tombstone_lifetime_ms;
                unregistered                                  = true;
                break;
            }
        }
        if (!unregistered &&
            shared_state_->unregistration_tombstones.find(key) != shared_state_->unregistration_tombstones.end())
            unregistered = true;
    }
    if (unregistered) {
        (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_UNREGISTERED,
                          unregister_message.registration_token, sizeof(unregister_message.registration_token));
        return;
    }
    (void)sendRejected(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_UNREGISTER,
                       unregister_message.registration_token, sizeof(unregister_message.registration_token),
                       k_rejection_token_invalid);
}

void NtrsServer::handleRequest(UdpSocket* socket, const sockaddr_storage& peer, socklen_t peer_length,
                               const utp_ntrs_endpoint_t& observed, const utp_frame_rendezvous_t& frame)
{
    utp_rendezvous_request_t        request                            = {};
    utp_rendezvous_candidate_plan_t target_plan                        = {};
    utp_rendezvous_redirect_t       redirect                           = {};
    sockaddr_storage                target_address                     = {};
    socklen_t                       target_length                      = 0u;
    char                            source_text[INET6_ADDRSTRLEN + 8u] = {};
    char                            target_text[INET6_ADDRSTRLEN + 8u] = {};
    PendingRendezvous               transaction                        = {};
    PendingRendezvous               existing_transaction               = {};
    std::string                     target_peer_id;
    std::string                     rendezvous_key;
    bool                            rejected            = false;
    bool                            existing            = false;
    bool                            prepare_calibration = false;
    bool                            prepare_rendezvous  = false;
    uint16_t                        rejection_reason    = 0u;

    assert(shared_state_ != NULL);
    if (utp_rendezvous_request_decode(&request, frame.payload, frame.payload_length) != UTP_INTERNAL_ERROR_OK ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV4 && observed.family != AF_INET) ||
        (request.local_family == UTP_ADDRESS_FAMILY_IPV6 && observed.family != AF_INET6))
        return;
    rendezvous_key = std::string(reinterpret_cast<const char*>(request.rendezvous_id), sizeof(request.rendezvous_id));
    target_peer_id = std::string(reinterpret_cast<const char*>(request.target_peer_id), request.target_peer_id_length);
    {
        std::unordered_map<std::string, PendingRendezvous>::iterator pending = pending_rendezvous_.find(rendezvous_key);
        if (pending != pending_rendezvous_.end() && !isRegistrationTokenActive(pending->second.target_token)) {
            erasePendingRendezvous(rendezvous_key);
            pending = pending_rendezvous_.end();
        }
        if (pending != pending_rendezvous_.end()) {
            if (endpoints_equal(pending->second.source_endpoint, observed)) {
                existing_transaction = pending->second;
                existing             = true;
            }
        } else {
            Registration target_registration = {};
            bool         target_found        = false;
            {
                std::lock_guard<std::mutex>                                   lock(shared_state_->mutex);
                std::unordered_map<std::string, Registration>::const_iterator target =
                    shared_state_->registrations.find(target_peer_id);
                if (target != shared_state_->registrations.end()) {
                    target_registration = target->second;
                    target_found        = true;
                }
            }
            if (!target_found || target_registration.local_family != request.local_family ||
                !make_candidate_plan(
                    &target_plan, target_registration.local_family, target_registration.local_port,
                    target_registration.local_candidates.data(), target_registration.local_candidate_count,
                    target_registration.has_reported_public_endpoint ? &target_registration.reported_public_endpoint
                                                                     : NULL,
                    target_registration.endpoint, public_candidate_count_) ||
                !appendRegistrationObservedCandidates(&target_plan, target_registration, public_candidate_count_) ||
                !appendRegistrationPredictedCandidates(&target_plan, target_registration, public_candidate_count_) ||
                !utp_ntrs_endpoint_to_sockaddr(&target_registration.endpoint, &target_address, &target_length)) {
                rejected         = true;
                rejection_reason = k_rejection_peer_not_found;
            } else {
                if (!random_token(&transaction.punch_token)) return;
                memcpy(transaction.rendezvous_id.data(), request.rendezvous_id, transaction.rendezvous_id.size());
                memcpy(redirect.rendezvous_id, request.rendezvous_id, sizeof(redirect.rendezvous_id));
                memcpy(redirect.punch_token, transaction.punch_token.data(), transaction.punch_token.size());
                redirect.target_plan = target_plan;
                if (utp_rendezvous_redirect_encode(transaction.redirect_body.data(), transaction.redirect_body.size(),
                                                   &redirect,
                                                   &transaction.redirect_body_length) != UTP_INTERNAL_ERROR_OK)
                    return;
                transaction.source_endpoint       = observed;
                transaction.target_endpoint       = target_registration.endpoint;
                transaction.source_socket_address = peer;
                transaction.source_socket_length  = peer_length;
                transaction.target_socket_address = target_address;
                transaction.target_socket_length  = target_length;
                memcpy(transaction.target_token.data(), target_registration.token.data(),
                       transaction.target_token.size());
                memcpy(transaction.source_peer_id.data(), request.source_peer_id, request.source_peer_id_length);
                transaction.source_peer_id_length               = request.source_peer_id_length;
                transaction.source_local_port                   = request.local_port;
                transaction.source_local_family                 = request.local_family;
                transaction.source_local_candidate_count        = request.local_candidate_count;
                transaction.has_source_reported_public_endpoint = request.reported_public_endpoint != NULL;
                if (transaction.has_source_reported_public_endpoint)
                    transaction.source_reported_public_endpoint = *request.reported_public_endpoint;
                for (uint8_t index = 0u; index < request.local_candidate_count; ++index)
                    transaction.source_local_candidates[index] = request.local_candidates[index];
                transaction.created_at_ms       = utp_ntrs_now_ms();
                transaction.calibration_pending = request.source_nat_class == UTP_NAT_CLASS_SYMMETRIC &&
                                                  target_registration.nat_class == UTP_NAT_CLASS_PORT_RESTRICTED &&
                                                  calibration_endpoint_count_ >= k_temporary_calibration_endpoint_count;
                if (transaction.calibration_pending) {
                    transaction.calibration_ip            = observed;
                    transaction.calibration_ports[0u]     = observed.port;
                    transaction.calibration_received_mask = UINT8_C(0x01);
                    transaction.calibration_ip_consistent = true;
                    transaction.calibration_deadline_ms =
                        transaction.created_at_ms + k_temporary_calibration_timeout_ms;
                    if (!random_u64(&transaction.calibration_id) || !random_token(&transaction.calibration_token))
                        return;
                    prepare_calibration = true;
                } else {
                    prepare_rendezvous = true;
                }
                pending_rendezvous_.insert(std::make_pair(rendezvous_key, transaction));
            }
        }
    }
    if (rejected) {
        (void)sendRejected(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REQUEST, request.rendezvous_id,
                           sizeof(request.rendezvous_id), rejection_reason);
        return;
    }
    if (existing) {
        if (!isRegistrationTokenActive(existing_transaction.target_token)) {
            erasePendingRendezvous(rendezvous_key);
            (void)sendRejected(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REQUEST, request.rendezvous_id,
                               sizeof(request.rendezvous_id), k_rejection_peer_not_found);
            return;
        }
        if (existing_transaction.calibration_pending) {
            CalibrationStartTask start = {};

            start.rendezvous_id     = existing_transaction.rendezvous_id;
            start.calibration_token = existing_transaction.calibration_token;
            start.observed          = existing_transaction.calibration_ip;
            start.calibration_id    = existing_transaction.calibration_id;
            start.deadline_ms       = existing_transaction.calibration_deadline_ms;
            start.owner_worker      = worker_index_;
            (void)enqueueCalibrationStart(start);
        } else {
            (void)sendMessage(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REDIRECT,
                              existing_transaction.redirect_body.data(), existing_transaction.redirect_body_length);
        }
        return;
    }
    if (prepare_calibration) {
        if (!isRegistrationTokenActive(transaction.target_token)) {
            erasePendingRendezvous(rendezvous_key);
            (void)sendRejected(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REQUEST, request.rendezvous_id,
                               sizeof(request.rendezvous_id), k_rejection_peer_not_found);
            return;
        }
        CalibrationStartTask start = {};

        start.rendezvous_id     = transaction.rendezvous_id;
        start.calibration_token = transaction.calibration_token;
        start.observed          = transaction.calibration_ip;
        start.calibration_id    = transaction.calibration_id;
        start.deadline_ms       = transaction.calibration_deadline_ms;
        start.owner_worker      = worker_index_;
        if (!enqueueCalibrationStart(start)) {
            erasePendingRendezvous(rendezvous_key);
            return;
        }
        fprintf(stderr, "NTRS <- Node=%s [Request|Calibrate] target=%s\n",
                utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)), target_peer_id.c_str());
        return;
    }
    if (!prepare_rendezvous) return;
    if (!isRegistrationTokenActive(transaction.target_token)) {
        erasePendingRendezvous(rendezvous_key);
        (void)sendRejected(socket, peer, peer_length, UTP_RENDEZVOUS_MESSAGE_REQUEST, request.rendezvous_id,
                           sizeof(request.rendezvous_id), k_rejection_peer_not_found);
        return;
    }
    PreparedDatagram redirect_datagram = {};
    PreparedDatagram forward_datagram  = {};
    if (!preparePendingRendezvous(&transaction, &redirect_datagram, &forward_datagram)) return;
    (void)sendDatagram(redirect_datagram);
    std::unordered_map<std::string, PendingRendezvous>::iterator pending = pending_rendezvous_.find(rendezvous_key);
    if (pending == pending_rendezvous_.end() || pending->second.forward_packet_number != 0u) return;
    pending->second = transaction;
    {
        std::lock_guard<std::mutex> lock(shared_state_->mutex);
        shared_state_
            ->forward_owners[rendezvous_route_key(transaction.target_token.data(), transaction.forward_packet_number)] =
            worker_index_;
    }
    (void)sendDatagram(forward_datagram);
    fprintf(stderr, "NTRS <- Node=%s [Request] target_peer_id=%s\n",
            utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)), target_peer_id.c_str());
    fprintf(stderr, "NTRS -> Node=%s [Redirect] target=%s\n",
            utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)), target_peer_id.c_str());
    fprintf(stderr, "NTRS -> Node=%s [Ping|Forward] source=%s\n",
            utp_ntrs_endpoint_format(&transaction.target_endpoint, target_text, sizeof(target_text)),
            utp_ntrs_endpoint_format(&observed, source_text, sizeof(source_text)));
}

void NtrsServer::OnReadEvent(evutil_socket_t, short, void* user_data)
{
    UdpSocket* const socket = static_cast<UdpSocket*>(user_data);

    assert(socket != NULL);
    assert(socket->server != NULL);
    socket->server->onRead(socket);
}

void NtrsServer::OnControlEvent(evutil_socket_t, short, void* user_data)
{
    assert(user_data != NULL);
    static_cast<NtrsServer*>(user_data)->onControlEvent();
}

void NtrsServer::OnControlOutputEvent(evutil_socket_t, short, void* user_data)
{
    UdpSocket* const socket = static_cast<UdpSocket*>(user_data);

    assert(socket != NULL);
    assert(socket->server != NULL);
    socket->server->onControlOutput(socket);
}

void NtrsServer::OnWorkerEvent(evutil_socket_t, short, void* user_data)
{
    assert(user_data != NULL);
    static_cast<NtrsServer*>(user_data)->onWorkerEvent();
}

void NtrsServer::onWorkerEvent()
{
    uint8_t discarded[64];

    while (read(worker_notify_fd_, discarded, sizeof(discarded)) > 0) {
    }
    for (;;) {
        WorkerTask task = {};

        if (!worker_queue_.pop(&task)) return;
        const utp_frame_rendezvous_t frame = {task.frame_payload.data(), task.frame_payload_length, task.message_type};
        if (task.task_type == kWorkerTaskRequest)
            handleRequest(task.socket, task.peer, task.peer_length, task.observed, frame);
        else if (task.task_type == kWorkerTaskForwardPong)
            handleForwardPong(task.observed, frame);
        else if (task.task_type == kWorkerTaskCalibrationAccepted)
            handleCalibrationAccepted(task);
    }
}

void NtrsServer::onRead(UdpSocket* socket)
{
    assert(socket != NULL);
    assert(socket->fd >= 0);
    for (;;) {
        uint8_t          packet[UTP_PACKET_MTU_FLOOR];
        sockaddr_storage peer        = {};
        socklen_t        peer_length = sizeof(peer);
        const ssize_t    count =
            recvfrom(socket->fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        utp_packet_header_t header   = {};
        utp_ntrs_endpoint_t observed = {};

        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) fprintf(stderr, "event=udp_receive_failed errno=%d\n", errno);
            return;
        }
        if (utp_proto_decode_header(&header, packet, static_cast<size_t>(count)) != UTP_INTERNAL_ERROR_OK ||
            header.packet_number == 0u || header.reserve != 0u ||
            static_cast<size_t>(count) != UTP_PACKET_HEADER_SIZE + header.payload_length ||
            !utp_ntrs_endpoint_from_sockaddr(&observed, reinterpret_cast<const sockaddr*>(&peer), peer_length))
            continue;
        if (header.type == UTP_PACKET_TYPE_RENDEZVOUS) {
            utp_frame_rendezvous_t frame = {};
            if (header.scid != 0u || header.dcid != 0u ||
                utp_frame_rendezvous_decode(&frame, packet + UTP_PACKET_HEADER_SIZE, header.payload_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                header.payload_length != UTP_FRAME_RENDEZVOUS_HEADER_SIZE + frame.payload_length)
                continue;
            if (socket->calibration_index != k_primary_socket_index) {
                if (frame.message_type != UTP_RENDEZVOUS_MESSAGE_PING) continue;
                utp_rendezvous_ping_t ping = {};

                if (utp_rendezvous_ping_decode(&ping, frame.payload, frame.payload_length) !=
                    UTP_INTERNAL_ERROR_OK)
                    continue;
                if (ping.calibration_id != 0u)
                    handleCalibrationPing(socket, peer, peer_length, observed, header, frame);
                else
                    handlePing(socket, peer, peer_length, observed, header, frame);
                continue;
            }
            if (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PONG &&
                socket->calibration_index == k_primary_socket_index) {
                utp_rendezvous_pong_t pong  = {};
                NtrsServer*           owner = NULL;

                if (utp_rendezvous_pong_decode(&pong, frame.payload, frame.payload_length) == UTP_INTERNAL_ERROR_OK) {
                    std::lock_guard<std::mutex>                               lock(shared_state_->mutex);
                    std::unordered_map<std::string, uint16_t>::const_iterator route =
                        shared_state_->forward_owners.find(
                            rendezvous_route_key(pong.registration_token, pong.acknowledged_packet_number));
                    if (route != shared_state_->forward_owners.end() && route->second < shared_state_->workers.size())
                        owner = shared_state_->workers[route->second];
                }
                if (owner != NULL) {
                    if (owner == this) {
                        handleForwardPong(observed, frame);
                    } else {
                        WorkerTask task           = {};
                        task.observed             = observed;
                        task.message_type         = frame.message_type;
                        task.task_type            = kWorkerTaskForwardPong;
                        task.frame_payload_length = frame.payload_length;
                        memcpy(task.frame_payload.data(), frame.payload, frame.payload_length);
                        (void)owner->enqueueWorker(task);
                    }
                    continue;
                }
            }
            const bool supported = (frame.message_type == UTP_RENDEZVOUS_MESSAGE_REGISTER &&
                                    socket->calibration_index == k_primary_socket_index) ||
                                   (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PING &&
                                    (socket->calibration_index == k_primary_socket_index ||
                                     socket->calibration_index < k_temporary_calibration_endpoint_count)) ||
                                   (frame.message_type == UTP_RENDEZVOUS_MESSAGE_PONG &&
                                    socket->calibration_index == k_primary_socket_index) ||
                                   (frame.message_type == UTP_RENDEZVOUS_MESSAGE_ADDRESS_UPDATE &&
                                    socket->calibration_index == k_primary_socket_index) ||
                                   (frame.message_type == UTP_RENDEZVOUS_MESSAGE_UNREGISTER &&
                                    socket->calibration_index == k_primary_socket_index);
            if (supported) {
                ControlTask task          = {};
                task.peer                 = peer;
                task.peer_length          = peer_length;
                task.observed             = observed;
                task.header               = header;
                task.socket               = socket;
                task.message_type         = frame.message_type;
                task.frame_payload_length = frame.payload_length;
                memcpy(task.frame_payload.data(), frame.payload, frame.payload_length);
                (void)enqueueControl(task);
            }
        } else if (socket->calibration_index == k_primary_socket_index &&
                   (header.type == UTP_PACKET_TYPE_INITIAL || header.type == UTP_PACKET_TYPE_0RTT)) {
            utp_frame_rendezvous_t frame        = {};
            uint8_t                frame_type   = 0u;
            size_t                 frame_length = 0u;
            if (header.scid == 0u || header.dcid != 0u ||
                utp_frame_measure(packet + UTP_PACKET_HEADER_SIZE, header.payload_length, &frame_type, &frame_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                frame_type != UTP_FRAME_TYPE_RENDEZVOUS ||
                utp_frame_rendezvous_decode(&frame, packet + UTP_PACKET_HEADER_SIZE, frame_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                frame.message_type != UTP_RENDEZVOUS_MESSAGE_REQUEST)
                continue;
            utp_rendezvous_request_t request_hint = {};
            NtrsServer*              owner;

            if (utp_rendezvous_request_decode(&request_hint, frame.payload, frame.payload_length) !=
                    UTP_INTERNAL_ERROR_OK ||
                shared_state_ == NULL || shared_state_->workers.empty())
                continue;
            owner = shared_state_->workers[rendezvous_hash(
                request_hint.rendezvous_id, sizeof(request_hint.rendezvous_id), shared_state_->workers.size())];
            if (owner == this) {
                handleRequest(socket, peer, peer_length, observed, frame);
                continue;
            }
            WorkerTask task           = {};
            task.peer                 = peer;
            task.peer_length          = peer_length;
            task.observed             = observed;
            task.header               = header;
            task.socket               = socket;
            task.message_type         = frame.message_type;
            task.task_type            = kWorkerTaskRequest;
            task.frame_payload_length = frame.payload_length;
            memcpy(task.frame_payload.data(), frame.payload, frame.payload_length);
            (void)owner->enqueueWorker(task);
        }
    }
}

static void on_signal(int) { g_stop_requested = 1; }

void        NtrsServer::OnTimerEvent(evutil_socket_t, short, void* user_data)
{
    assert(user_data != NULL);
    static_cast<NtrsServer*>(user_data)->onTimer();
}

void NtrsServer::OnControlTimerEvent(evutil_socket_t, short, void* user_data)
{
    assert(user_data != NULL);
    static_cast<NtrsServer*>(user_data)->onControlTimer();
}

void NtrsServer::handleControlTask(const ControlTask& task)
{
    const utp_frame_rendezvous_t frame = {task.frame_payload.data(), task.frame_payload_length, task.message_type};

    if (task.message_type == UTP_RENDEZVOUS_MESSAGE_REGISTER)
        handleRegister(task.socket, task.peer, task.peer_length, task.observed, frame);
    else if (task.message_type == UTP_RENDEZVOUS_MESSAGE_PING)
        handlePing(task.socket, task.peer, task.peer_length, task.observed, task.header, frame);
    else if (task.message_type == UTP_RENDEZVOUS_MESSAGE_PONG)
        handlePong(task.observed, frame);
    else if (task.message_type == UTP_RENDEZVOUS_MESSAGE_ADDRESS_UPDATE)
        handleAddressUpdate(task.socket, task.peer, task.peer_length, task.observed, frame);
    else if (task.message_type == UTP_RENDEZVOUS_MESSAGE_UNREGISTER)
        handleUnregister(task.socket, task.peer, task.peer_length, frame);
}

void NtrsServer::onControlEvent()
{
    uint64_t discarded;

    assert(shared_state_ != NULL);
    while (read(shared_state_->control_notify_fd, &discarded, sizeof(discarded)) < 0 && errno == EINTR) {
    }
    for (;;) {
        ControlTask task = {};
        if (shared_state_->lifecycle_control_queue.pop(&task) || shared_state_->keepalive_control_queue.pop(&task)) {
            handleControlTask(task);
            continue;
        }
        CalibrationStartTask calibration_start = {};
        if (shared_state_->calibration_start_queue.pop(&calibration_start)) {
            handleCalibrationStart(calibration_start);
            continue;
        }
        return;
    }
}

void NtrsServer::onControlOutput(UdpSocket* socket)
{
    uint64_t discarded;

    assert(socket != NULL);
    while (read(socket->control_output_read_fd, &discarded, sizeof(discarded)) < 0 && errno == EINTR) {
    }
    for (;;) {
        OutgoingDatagram output = {};
        if (!socket->control_output_queue.pop(&output)) break;

        if (socket->output_queue.size() >= output_queue_capacity_) {
            fprintf(stderr, "NTRS [DatagramDropped] output queue full\n");
            socket->control_output_queue_size.fetch_sub(1u, std::memory_order_relaxed);
        } else {
            socket->output_queue.push_back(output);
        }
    }
    if (!socket->output_queue.empty() && !socket->write_event_active) {
        (void)event_add(socket->write_event, NULL);
        socket->write_event_active = true;
    }
}

void NtrsServer::onTimer()
{
    assert(shared_state_ != NULL);
    if (g_stop_requested != 0 || shared_state_->fatal_socket_error.load(std::memory_order_relaxed)) {
        event_base_loopbreak(base_);
        return;
    }
    const uint64_t now_ms = utp_ntrs_now_ms();
    for (std::unordered_map<std::string, PendingRendezvous>::iterator entry = pending_rendezvous_.begin();
         entry != pending_rendezvous_.end();) {
        PendingRendezvous& transaction = entry->second;

        if (!isRegistrationTokenActive(transaction.target_token) ||
            now_ms - transaction.created_at_ms >= k_pending_rendezvous_lifetime_ms) {
            std::lock_guard<std::mutex> lock(shared_state_->mutex);
            if (transaction.forward_packet_number != 0u)
                shared_state_->forward_owners.erase(
                    rendezvous_route_key(transaction.target_token.data(), transaction.forward_packet_number));
            entry = pending_rendezvous_.erase(entry);
            continue;
        }
        if (!transaction.calibration_pending && !transaction.forward_delivered &&
            transaction.forward_retries_remaining != 0u &&
                   transaction.forward_retry_at_ms <= now_ms) {
            PreparedDatagram datagram = {};
            datagram.socket           = &primary_socket_;
            datagram.peer             = transaction.target_socket_address;
            datagram.peer_length      = transaction.target_socket_length;
            datagram.packet           = transaction.forward_packet;
            datagram.packet_length    = transaction.forward_packet_length;
            if (sendDatagram(datagram)) {
                --transaction.forward_retries_remaining;
                transaction.forward_retry_at_ms = now_ms + transaction.forward_retry_delay_ms;
                if (transaction.forward_retry_delay_ms < k_forward_retry_max_delay_ms / 2u)
                    transaction.forward_retry_delay_ms *= 2u;
                else
                    transaction.forward_retry_delay_ms = k_forward_retry_max_delay_ms;
            } else {
                transaction.forward_retry_at_ms = now_ms + k_forward_retry_send_failure_delay_ms;
            }
        }
        ++entry;
    }
}

void NtrsServer::onControlTimer()
{
    const uint64_t now_ms = utp_ntrs_now_ms();

    assert(shared_state_ != NULL);
    if (g_stop_requested != 0 || shared_state_->fatal_socket_error.load(std::memory_order_relaxed)) {
        event_base_loopbreak(control_base_);
        return;
    }
    for (std::unordered_map<std::string, CalibrationSession>::iterator entry = calibration_sessions_.begin();
         entry != calibration_sessions_.end();) {
        CalibrationSession& session = entry->second;

        if (session.completed) {
            if (!session.result_delivered && enqueueCalibrationResult(session))
                session.result_delivered = true;
            if (now_ms - session.completed_at_ms >= k_pending_rendezvous_lifetime_ms)
                entry = calibration_sessions_.erase(entry);
            else
                ++entry;
            continue;
        }
        if (session.deadline_ms <= now_ms) {
            session.completed       = true;
            session.completed_at_ms = now_ms;
            if (enqueueCalibrationResult(session)) session.result_delivered = true;
        }
        ++entry;
    }
    std::lock_guard<std::mutex> lock(shared_state_->mutex);
    expireRegistrations(now_ms);
    for (std::unordered_map<std::string, Registration>::iterator entry = shared_state_->registrations.begin();
         entry != shared_state_->registrations.end(); ++entry) {
        Registration& registration = entry->second;

        if (now_ms - registration.last_activity_ms >= keepalive_interval_ms_ &&
            (registration.last_keepalive_ping_ms == 0u ||
             now_ms - registration.last_keepalive_ping_ms >= keepalive_interval_ms_)) {
            PreparedDatagram datagram      = {};
            uint64_t         packet_number = 0u;

            if (prepareKeepalivePing(registration, &datagram, &packet_number) && sendDatagram(datagram)) {
                registration.keepalive_packet_number = packet_number;
                registration.last_keepalive_ping_ms  = now_ms;
            }
        }
    }
}

void NtrsServer::OnWriteEvent(evutil_socket_t, short, void* user_data)
{
    UdpSocket* const socket = static_cast<UdpSocket*>(user_data);

    assert(socket != NULL);
    assert(socket->server != NULL);
    socket->server->onWrite(socket);
}

void NtrsServer::onWrite(UdpSocket* socket)
{
    assert(socket != NULL);
    assert(socket->fd >= 0);
    while (!socket->output_queue.empty()) {
        const OutgoingDatagram& datagram = socket->output_queue.front();
        ssize_t                 sent_length;
        do {
            sent_length = sendto(socket->fd, datagram.packet.data(), datagram.packet_length, 0,
                                 reinterpret_cast<const sockaddr*>(&datagram.peer), datagram.peer_length);
        } while (sent_length < 0 && errno == EINTR);
        if (sent_length == static_cast<ssize_t>(datagram.packet_length)) {
            socket->output_queue.pop_front();
            socket->control_output_queue_size.fetch_sub(1u, std::memory_order_relaxed);
            continue;
        }
        if (sent_length >= 0) {
            fprintf(stderr, "NTRS [DatagramDropped] udp send short=%zd\n", sent_length);
            socket->output_queue.pop_front();
            socket->control_output_queue_size.fetch_sub(1u, std::memory_order_relaxed);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (is_fatal_udp_send_error(errno)) {
            stopForFatalError(errno, "udp send");
            return;
        }
        fprintf(stderr, "NTRS [DatagramDropped] udp send errno=%d\n", errno);
        socket->output_queue.pop_front();
        socket->control_output_queue_size.fetch_sub(1u, std::memory_order_relaxed);
    }
    event_del(socket->write_event);
    socket->write_event_active = false;
}

bool NtrsServer::start(const utp_ntrs_endpoint_t& bind_endpoint, const utp_ntrs_endpoint_t& advertised_endpoint,
                       const char* interface_name, const std::vector<uint16_t>& calibration_ports,
                       uint8_t public_candidate_count, uint32_t registration_timeout_ms, uint32_t keepalive_interval_ms,
                       size_t output_queue_capacity, size_t worker_queue_capacity)
{
    int worker_pair[2] = {-1, -1};

    if (bind_endpoint.family != advertised_endpoint.family || public_candidate_count == 0u ||
        registration_timeout_ms == 0u || keepalive_interval_ms == 0u ||
        keepalive_interval_ms >= registration_timeout_ms || output_queue_capacity == 0u ||
        worker_queue_capacity == 0u || public_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        calibration_ports.size() > calibration_sockets_.size() ||
        (!calibration_ports.empty() && endpoint_is_unspecified(advertised_endpoint)))
        return false;
    output_queue_capacity_ = output_queue_capacity;
    bind_endpoint_         = bind_endpoint;
    advertised_endpoint_   = advertised_endpoint;
    interface_name_        = interface_name == NULL ? std::string() : std::string(interface_name);
    calibration_ports_     = calibration_ports;
    if (!worker_queue_.create(worker_queue_capacity) || (base_ = event_base_new()) == NULL ||
        !createSocket(&primary_socket_, bind_endpoint, interface_name, k_primary_socket_index, base_) ||
        (timer_event_ = event_new(base_, -1, EV_PERSIST, OnTimerEvent, this)) == NULL ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, worker_pair) != 0 || evutil_make_socket_nonblocking(worker_pair[0]) != 0 ||
        evutil_make_socket_nonblocking(worker_pair[1]) != 0 ||
        (worker_event_ = event_new(base_, worker_pair[0], EV_READ | EV_PERSIST, OnWorkerEvent, this)) == NULL ||
        event_add(worker_event_, NULL) != 0) {
        if (worker_pair[0] >= 0) close(worker_pair[0]);
        if (worker_pair[1] >= 0) close(worker_pair[1]);
        return false;
    }
    worker_notify_fd_        = worker_pair[0];
    worker_notify_write_fd_  = worker_pair[1];
    public_candidate_count_  = public_candidate_count;
    registration_timeout_ms_ = registration_timeout_ms;
    keepalive_interval_ms_   = keepalive_interval_ms;
    for (size_t index = 0u; index < calibration_ports.size(); ++index) {
        utp_ntrs_endpoint_t calibration_bind_endpoint       = bind_endpoint;
        utp_ntrs_endpoint_t calibration_advertised_endpoint = advertised_endpoint;

        if (calibration_ports[index] == 0u || calibration_ports[index] == bind_endpoint.port) return false;
        for (size_t previous = 0u; previous < index; ++previous) {
            if (calibration_ports[previous] == calibration_ports[index]) return false;
        }
        calibration_bind_endpoint.port       = calibration_ports[index];
        calibration_advertised_endpoint.port = calibration_ports[index];
        if (!endpoint_to_address(calibration_advertised_endpoint, &calibration_endpoints_[index]))
            return false;
        ++calibration_endpoint_count_;
    }
    return true;
}

int NtrsServer::runControl()
{
    const timeval timer_interval = {0, 100000};

    if (shared_state_ == NULL || shared_state_->control_notify_fd < 0 || worker_index_ != 0u ||
        (control_base_ = event_base_new()) == NULL ||
        (control_event_ = event_new(control_base_, shared_state_->control_notify_fd, EV_READ | EV_PERSIST,
                                    OnControlEvent, this)) == NULL ||
        (control_timer_event_ = event_new(control_base_, -1, EV_PERSIST, OnControlTimerEvent, this)) == NULL ||
        event_add(control_event_, NULL) != 0 || event_add(control_timer_event_, &timer_interval) != 0) {
        if (shared_state_ != NULL) shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
        return 1;
    }
    for (size_t index = 0u; index < calibration_ports_.size(); ++index) {
        utp_ntrs_endpoint_t calibration_bind_endpoint = bind_endpoint_;

        calibration_bind_endpoint.port = calibration_ports_[index];
        if (!createSocket(&calibration_sockets_[index], calibration_bind_endpoint,
                          interface_name_.empty() ? NULL : interface_name_.c_str(),
                          static_cast<uint8_t>(index), control_base_)) {
            shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
            return 1;
        }
    }
    fprintf(stderr, "NTRS [ControlStarted]\n");
    int result = 0;
    while (g_stop_requested == 0 && !shared_state_->fatal_socket_error.load(std::memory_order_relaxed)) {
        result = event_base_dispatch(control_base_);
        if (result != 0) {
            const int error    = errno != 0 ? errno : EIO;
            int       expected = 0;
            if (shared_state_->fatal_error_code.compare_exchange_strong(expected, error, std::memory_order_relaxed,
                                                                        std::memory_order_relaxed)) {
                fprintf(stderr, "NTRS [Fatal] operation=control event loop errno=%d description=%s\n", error,
                        strerror(error));
            }
            shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
            break;
        }
    }

    (void)event_del(control_timer_event_);
    (void)event_del(control_event_);
    return result == 0 && (g_stop_requested != 0 || shared_state_->fatal_socket_error.load(std::memory_order_relaxed))
               ? 0
               : (g_stop_requested != 0 ? 0 : 1);
}

int NtrsServer::run(const utp_ntrs_endpoint_t& endpoint)
{
    const timeval timer_interval                            = {0, 100000};
    char          formatted_endpoint[INET6_ADDRSTRLEN + 8u] = {};

    if (event_add(timer_event_, &timer_interval) != 0) {
        if (shared_state_ != NULL) shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
        return 1;
    }
    fprintf(stderr, "NTRS [Started] worker=%u bind=%s\n", static_cast<unsigned>(worker_index_),
            utp_ntrs_endpoint_format(&endpoint, formatted_endpoint, sizeof(formatted_endpoint)));
    int result = 0;
    while (g_stop_requested == 0 && !fatal_socket_error_ && shared_state_ != NULL &&
           !shared_state_->fatal_socket_error.load(std::memory_order_relaxed)) {
        result = event_base_dispatch(base_);
        if (result != 0) {
            const int error = errno != 0 ? errno : EIO;
            stopForFatalError(error, "worker event loop");
            break;
        }
    }
    (void)event_del(timer_event_);
    return result != 0 || fatal_socket_error_ || shared_state_ == NULL ||
                   shared_state_->fatal_socket_error.load(std::memory_order_relaxed)
               ? 1
               : 0;
}

class NtrsRuntime
{
public:
    NtrsRuntime() : shared_state_(new SharedNtrsState()), workers_(), threads_(), control_thread_(), run_failed_(false)
    {
    }

    ~NtrsRuntime()
    {
        if (shared_state_->control_notify_fd >= 0) close(shared_state_->control_notify_fd);
        if (shared_state_->control_notify_write_fd >= 0) close(shared_state_->control_notify_write_fd);
        shared_state_->control_notify_fd       = -1;
        shared_state_->control_notify_write_fd = -1;
    }

    bool start(const utp_ntrs_endpoint_t& bind_endpoint, const utp_ntrs_endpoint_t& advertised_endpoint,
               const char* interface_name, const std::vector<uint16_t>& calibration_ports,
               uint8_t public_candidate_count, uint32_t registration_timeout_ms, uint32_t keepalive_interval_ms,
               uint16_t worker_count, size_t control_queue_capacity, size_t output_queue_capacity,
               size_t worker_queue_capacity)
    {
        int          control_pair[2] = {-1, -1};
        const size_t lifecycle_queue_capacity =
            control_queue_capacity / 4u + (control_queue_capacity % 4u == 0u ? 0u : 1u);
        const size_t keepalive_queue_capacity = control_queue_capacity - lifecycle_queue_capacity;

        if (worker_count == 0u || control_queue_capacity < 2u || output_queue_capacity == 0u ||
            worker_queue_capacity == 0u || shared_state_->control_notify_fd >= 0 ||
            !shared_state_->lifecycle_control_queue.create(lifecycle_queue_capacity) ||
            !shared_state_->keepalive_control_queue.create(keepalive_queue_capacity) ||
            !shared_state_->calibration_start_queue.create(control_queue_capacity))
            return false;
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, control_pair) != 0) return false;
        if (evutil_make_socket_nonblocking(control_pair[0]) != 0 ||
            evutil_make_socket_nonblocking(control_pair[1]) != 0) {
            close(control_pair[0]);
            close(control_pair[1]);
            return false;
        }
        shared_state_->control_notify_fd       = control_pair[0];
        shared_state_->control_notify_write_fd = control_pair[1];
        for (uint16_t index = 0u; index < worker_count; ++index) {
            std::unique_ptr<NtrsServer> worker(new NtrsServer(shared_state_, index));

            if (!worker->start(bind_endpoint, advertised_endpoint, interface_name, calibration_ports,
                               public_candidate_count, registration_timeout_ms, keepalive_interval_ms,
                               output_queue_capacity, worker_queue_capacity)) {
                close(shared_state_->control_notify_fd);
                close(shared_state_->control_notify_write_fd);
                shared_state_->control_notify_fd       = -1;
                shared_state_->control_notify_write_fd = -1;
                return false;
            }
            shared_state_->workers.push_back(worker.get());
            workers_.push_back(std::move(worker));
        }
        return true;
    }

    int run(const utp_ntrs_endpoint_t& endpoint)
    {
        NtrsServer* const controller = workers_.empty() ? NULL : workers_[0u].get();

        if (controller == NULL) return 1;
        control_thread_ = std::thread([this, controller]() {
            if (controller->runControl() != 0) {
                run_failed_.store(true, std::memory_order_relaxed);
                shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
            }
        });
        for (size_t index = 0u; index < workers_.size(); ++index) {
            NtrsServer* const worker = workers_[index].get();

            threads_.push_back(std::thread([this, worker, endpoint]() {
                if (worker->run(endpoint) != 0) {
                    run_failed_.store(true, std::memory_order_relaxed);
                    shared_state_->fatal_socket_error.store(true, std::memory_order_relaxed);
                }
            }));
        }
        control_thread_.join();
        for (size_t index = 0u; index < threads_.size(); ++index) {
            threads_[index].join();
        }
        ControlTask task = {};
        while (shared_state_->lifecycle_control_queue.pop(&task)) {
        }
        while (shared_state_->keepalive_control_queue.pop(&task)) {
        }
        CalibrationStartTask calibration_start = {};
        while (shared_state_->calibration_start_queue.pop(&calibration_start)) {
        }
        close(shared_state_->control_notify_fd);
        close(shared_state_->control_notify_write_fd);
        shared_state_->control_notify_fd       = -1;
        shared_state_->control_notify_write_fd = -1;
        return run_failed_.load(std::memory_order_relaxed) ||
                       shared_state_->fatal_socket_error.load(std::memory_order_relaxed)
                   ? 1
                   : 0;
    }

private:
    std::shared_ptr<SharedNtrsState>          shared_state_;
    std::vector<std::unique_ptr<NtrsServer> > workers_;
    std::vector<std::thread>                  threads_;
    std::thread                               control_thread_;
    std::atomic<bool>                         run_failed_;
};

int main(int argc, char** argv)
{
    CLI::App              cli{"NTRS rendezvous service"};
    std::string           bind_address = "0.0.0.0";
    std::string           advertised_address;
    std::string           interface_name;
    std::vector<uint16_t> calibration_ports;
    uint32_t              public_candidate_count  = UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES;
    uint32_t              registration_timeout_ms = k_registration_timeout_default_ms;
    uint32_t              keepalive_interval_ms   = k_keepalive_interval_default_ms;
    size_t                control_queue_capacity  = k_control_queue_capacity_default;
    size_t                output_queue_capacity   = k_output_queue_capacity_default;
    size_t                worker_queue_capacity   = k_worker_queue_capacity_default;
    uint16_t              port                    = 24000u;
    uint32_t              worker_count            = 1u;
    cli.add_option("-a", bind_address, "Bind IP");
    cli.add_option("-e", advertised_address, "Advertised public IP");
    cli.add_option("-p", port, "Bind UDP port");
    cli.add_option("-i", interface_name, "Bind interface");
    cli.add_option("-c", calibration_ports, "Calibration UDP port");
    cli.add_option("-n", public_candidate_count, "Public candidate count");
    cli.add_option("-t", registration_timeout_ms, "Registration timeout ms");
    cli.add_option("-k", keepalive_interval_ms, "NTRS keepalive interval ms");
    cli.add_option("-w,--workers", worker_count, "UDP SO_REUSEPORT worker count");
    cli.add_option("--control-queue-capacity", control_queue_capacity, "Control task queue capacity");
    cli.add_option("--output-queue-capacity", output_queue_capacity, "Per UDP socket output queue capacity");
    cli.add_option("--worker-queue-capacity", worker_queue_capacity, "Per Worker task queue capacity");
    CLI11_PARSE(cli, argc, argv);

    const std::string bind_endpoint_text = make_endpoint_text(bind_address, port);
    const std::string advertised_endpoint_text =
        make_endpoint_text(advertised_address.empty() ? bind_address : advertised_address, port);
    utp_ntrs_endpoint_t bind_endpoint       = {};
    utp_ntrs_endpoint_t advertised_endpoint = {};
    NtrsRuntime         server;

    if (public_candidate_count == 0u || public_candidate_count > UTP_RENDEZVOUS_MAX_LOCAL_CANDIDATES ||
        worker_count == 0u || worker_count > UINT16_MAX || registration_timeout_ms == 0u ||
        keepalive_interval_ms == 0u || keepalive_interval_ms >= registration_timeout_ms ||
        control_queue_capacity < 2u || output_queue_capacity == 0u || worker_queue_capacity == 0u ||
        !utp_ntrs_endpoint_parse(bind_endpoint_text.c_str(), &bind_endpoint) ||
        !utp_ntrs_endpoint_parse(advertised_endpoint_text.c_str(), &advertised_endpoint) ||
        !server.start(bind_endpoint, advertised_endpoint, interface_name.empty() ? NULL : interface_name.c_str(),
                      calibration_ports, static_cast<uint8_t>(public_candidate_count), registration_timeout_ms,
                      keepalive_interval_ms, static_cast<uint16_t>(worker_count), control_queue_capacity,
                      output_queue_capacity, worker_queue_capacity))
        return 1;
    utp_ntrs_app_log_init("ntrs");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    return server.run(bind_endpoint);
}
