#include <chrono>
#include <numeric>
#include <stdint.h>
#include <string.h>

#include "custom_cast.h"
#include "galea.h"
#include "timestamp.h"

#ifndef _WIN32
#include <errno.h>
#endif

constexpr int Galea::num_channels;
constexpr int Galea::package_size;
constexpr int Galea::num_packages;
constexpr int Galea::transaction_size;


Galea::Galea (struct BrainFlowInputParams params) : Board ((int)BoardIds::GALEA_BOARD, params)
{
    socket = NULL;
    is_streaming = false;
    keep_alive = false;
    initialized = false;
    state = (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR;
    time_delay = 0.0;
}

Galea::~Galea ()
{
    skip_logs = true;
    release_session ();
}

int Galea::prepare_session ()
{
    if (initialized)
    {
        safe_logger (spdlog::level::info, "Session is already prepared");
        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    if (params.ip_address.empty ())
    {
        safe_logger (spdlog::level::info, "use default IP address 192.168.4.1");
        params.ip_address = "192.168.4.1";
    }
    if (params.ip_protocol == (int)IpProtocolType::TCP)
    {
        safe_logger (spdlog::level::err, "ip protocol is UDP for novaxr");
        return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
    }
    socket = new SocketClientUDP (params.ip_address.c_str (), 2390);
    int res = socket->connect ();
    if (res != (int)SocketClientUDPReturnCodes::STATUS_OK)
    {
        safe_logger (spdlog::level::err, "failed to init socket: {}", res);
        delete socket;
        socket = NULL;
        return (int)BrainFlowExitCodes::GENERAL_ERROR;
    }
    // force default settings for device
    std::string tmp;
    std::string default_settings = "d";
    res = config_board (default_settings, tmp);
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        safe_logger (spdlog::level::err, "failed to apply default settings");
        delete socket;
        socket = NULL;
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    // force default sampling rate - 250
    std::string sampl_rate = "~6";
    res = config_board (sampl_rate, tmp);
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        safe_logger (spdlog::level::err, "failed to apply defaul sampling rate");
        delete socket;
        socket = NULL;
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    initialized = true;
    return (int)BrainFlowExitCodes::STATUS_OK;
}

int Galea::config_board (std::string conf, std::string &response)
{
    if (socket == NULL)
    {
        safe_logger (spdlog::level::err, "You need to call prepare_session before config_board");
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }
    const char *config = conf.c_str ();
    safe_logger (spdlog::level::debug, "Trying to config Galea with {}", config);
    int len = strlen (config);
    int res = socket->send (config, len);
    if (len != res)
    {
        if (res == -1)
        {
#ifdef _WIN32
            safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
            safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
        }
        safe_logger (spdlog::level::err, "Failed to config a board");
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }
    if (!is_streaming)
    {
        constexpr int max_string_size = 8192;
        char b[max_string_size];
        res = Galea::transaction_size;
        int max_attempt = 25; // to dont get to infinite loop
        int current_attempt = 0;
        while (res == Galea::transaction_size)
        {
            res = socket->recv (b, max_string_size);
            if (res == -1)
            {
#ifdef _WIN32
                safe_logger (spdlog::level::err, "config_board recv ack WSAGetLastError is {}",
                    WSAGetLastError ());
#else
                safe_logger (spdlog::level::err, "config_board recv ack errno {} message {}", errno,
                    strerror (errno));
#endif
                return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
            }
            current_attempt++;
            if (current_attempt == max_attempt)
            {
                safe_logger (spdlog::level::err, "Device is streaming data while it should not!");
                return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
            }
        }
        // set response string
        for (int i = 0; i < res; i++)
        {
            response = response + b[i];
        }
        switch (b[0])
        {
            case 'A':
                return (int)BrainFlowExitCodes::STATUS_OK;
            case 'I':
                safe_logger (spdlog::level::err, "invalid command");
                return (int)BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR;
            default:
                safe_logger (spdlog::level::warn, "unknown char received: {}", b[0]);
                return (int)BrainFlowExitCodes::STATUS_OK;
        }
    }

    return (int)BrainFlowExitCodes::STATUS_OK;
}

int Galea::start_stream (int buffer_size, char *streamer_params)
{
    if (!initialized)
    {
        safe_logger (spdlog::level::err, "You need to call prepare_session before config_board");
        return (int)BrainFlowExitCodes::BOARD_NOT_CREATED_ERROR;
    }
    if (is_streaming)
    {
        safe_logger (spdlog::level::err, "Streaming thread already running");
        return (int)BrainFlowExitCodes::STREAM_ALREADY_RUN_ERROR;
    }
    if (buffer_size <= 0 || buffer_size > MAX_CAPTURE_SAMPLES)
    {
        safe_logger (spdlog::level::err, "invalid array size");
        return (int)BrainFlowExitCodes::INVALID_BUFFER_SIZE_ERROR;
    }

    if (db)
    {
        delete db;
        db = NULL;
    }
    if (streamer)
    {
        delete streamer;
        streamer = NULL;
    }

    int res = prepare_streamer (streamer_params);
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return res;
    }
    db = new DataBuffer (Galea::num_channels, buffer_size);
    if (!db->is_ready ())
    {
        safe_logger (spdlog::level::err, "unable to prepare buffer");
        delete db;
        db = NULL;
        return (int)BrainFlowExitCodes::INVALID_BUFFER_SIZE_ERROR;
    }

    // calc delay before start stream
    res = calc_delay ();
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return res;
    }

    // start streaming
    res = socket->send ("b", 1);
    if (res != 1)
    {
        if (res == -1)
        {
#ifdef _WIN32
            safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
            safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
        }
        safe_logger (spdlog::level::err, "Failed to send a command to board");
        return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
    }

    keep_alive = true;
    streaming_thread = std::thread ([this] { this->read_thread (); });
    // wait for data to ensure that everything is okay
    std::unique_lock<std::mutex> lk (this->m);
    auto sec = std::chrono::seconds (1);
    if (cv.wait_for (lk, 5 * sec,
            [this] { return this->state != (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR; }))
    {
        this->is_streaming = true;
        return this->state;
    }
    else
    {
        safe_logger (spdlog::level::err, "no data received in 5sec, stopping thread");
        this->is_streaming = true;
        this->stop_stream ();
        return (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR;
    }
}

int Galea::stop_stream ()
{
    if (is_streaming)
    {
        keep_alive = false;
        is_streaming = false;
        streaming_thread.join ();
        if (streamer)
        {
            delete streamer;
            streamer = NULL;
        }
        this->state = (int)BrainFlowExitCodes::SYNC_TIMEOUT_ERROR;
        int res = socket->send ("s", 1);
        if (res != 1)
        {
            if (res == -1)
            {
#ifdef _WIN32
                safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
                safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
            }
            safe_logger (spdlog::level::err, "Failed to send a command to board");
            return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
        }

        // free kernel buffer
        socket->set_timeout (2);
        unsigned char b[Galea::transaction_size];
        res = 0;
        int max_attempt = 25; // to dont get to infinite loop
        int current_attempt = 0;
        while (res != -1)
        {
            res = socket->recv (b, Galea::transaction_size);
            current_attempt++;
            if (current_attempt == max_attempt)
            {
                safe_logger (
                    spdlog::level::err, "Command 's' was sent but streaming is still running.");
                socket->set_timeout (5);
                return (int)BrainFlowExitCodes::BOARD_WRITE_ERROR;
            }
        }
        socket->set_timeout (5);

        return (int)BrainFlowExitCodes::STATUS_OK;
    }
    else
    {
        return (int)BrainFlowExitCodes::STREAM_THREAD_IS_NOT_RUNNING;
    }
}

int Galea::release_session ()
{
    if (initialized)
    {
        if (is_streaming)
        {
            stop_stream ();
        }
        initialized = false;
        if (socket)
        {
            socket->close ();
            delete socket;
            socket = NULL;
        }
    }
    return (int)BrainFlowExitCodes::STATUS_OK;
}

void Galea::read_thread ()
{
    int res;
    unsigned char b[Galea::transaction_size];
    constexpr int offset_last_package = Galea::package_size * (Galea::num_packages - 1);
    for (int i = 0; i < Galea::transaction_size; i++)
    {
        b[i] = 0;
    }
    while (keep_alive)
    {
        res = socket->recv (b, Galea::transaction_size);
        double recv_time = get_timestamp () - time_delay;
        if (res == -1)
        {
#ifdef _WIN32
            safe_logger (spdlog::level::err, "WSAGetLastError is {}", WSAGetLastError ());
#else
            safe_logger (spdlog::level::err, "errno {} message {}", errno, strerror (errno));
#endif
        }
        if (res != Galea::transaction_size)
        {
            safe_logger (spdlog::level::trace, "unable to read {} bytes, read {}",
                Galea::transaction_size, res);
            if (res > 0)
            {
                // more likely its a string received, try to print it
                b[res] = '\0';
                safe_logger (spdlog::level::warn, "Received: {}", b);
            }
            continue;
        }
        else
        {
            // inform main thread that everything is ok and first package was received
            if (this->state != (int)BrainFlowExitCodes::STATUS_OK)
            {
                safe_logger (spdlog::level::info,
                    "received first package with {} bytes streaming is started", res);
                {
                    std::lock_guard<std::mutex> lk (this->m);
                    this->state = (int)BrainFlowExitCodes::STATUS_OK;
                }
                this->cv.notify_one ();
                safe_logger (spdlog::level::debug, "start streaming");
            }
        }

        for (int cur_package = 0; cur_package < Galea::num_packages; cur_package++)
        {
            double package[Galea::num_channels] = {0.};
            int offset = cur_package * package_size;
            // package num
            package[0] = (double)b[0 + offset];
            // eeg and emg
            for (int i = 4, tmp_counter = 0; i < 20; i++, tmp_counter++)
            {
                // put them directly after package num in brainflow
                if (tmp_counter < 8)
                    package[i - 3] = eeg_scale_main_board *
                        (double)cast_24bit_to_int32 (b + offset + 5 + 3 * (i - 4));
                else if ((tmp_counter == 9) || (tmp_counter == 14))
                    package[i - 3] = eeg_scale_sister_board *
                        (double)cast_24bit_to_int32 (b + offset + 5 + 3 * (i - 4));
                else
                    package[i - 3] =
                        emg_scale * (double)cast_24bit_to_int32 (b + offset + 5 + 3 * (i - 4));
            }
            uint16_t temperature;
            int32_t ppg_ir;
            int32_t ppg_red;
            float eda;
            memcpy (&temperature, b + 54 + offset, 2);
            memcpy (&eda, b + 1 + offset, 4);
            memcpy (&ppg_red, b + 56 + offset, 4);
            memcpy (&ppg_ir, b + 60 + offset, 4);
            // ppg
            package[17] = (double)ppg_red;
            package[18] = (double)ppg_ir;
            // eda
            package[19] = (double)eda;
            // temperature
            package[20] = temperature / 100.0;
            // battery
            package[21] = (double)b[53 + offset];

            double timestamp_device_cur;
            memcpy (&timestamp_device_cur, b + 64 + offset, 8);
            double timestamp_device_last;
            memcpy (&timestamp_device_last, b + 64 + offset_last_package, 8);
            timestamp_device_cur /= 1e6; // convert usec to sec
            timestamp_device_last /= 1e6;
            double time_delta = timestamp_device_last - timestamp_device_cur;

            // workaround micros() overflow issue in firmware
            double timestamp = (time_delta < 0) ? recv_time : recv_time - time_delta;

            streamer->stream_data (package, Galea::num_channels, timestamp);
            db->add_data (timestamp, package);
        }
    }
}

int Galea::calc_delay ()
{
    int num_repeats = 5;
    std::vector<double> times;
    int num_fails = 0;
    unsigned char b[Galea::transaction_size];

    for (int i = 0; i < num_repeats; i++)
    {
        auto started = std::chrono::high_resolution_clock::now ();
        int res = socket->send ("F4", 2);
        if (res != 2)
        {
            safe_logger (spdlog::level::warn, "failed to send time calc command to device");
            num_fails++;
            continue;
        }
        res = socket->recv (b, Galea::transaction_size);
        if (res != Galea::transaction_size)
        {
            safe_logger (spdlog::level::warn,
                "failed to recv resp from time calc command, resp size {}", res);
            num_fails++;
            continue;
        }
        auto done = std::chrono::high_resolution_clock::now ();
        double duration =
            (double)std::chrono::duration_cast<std::chrono::milliseconds> (done - started).count ();
        times.push_back (duration);
    }
    if (num_fails > 1)
    {
        safe_logger (spdlog::level::err,
            "Failed to calc time delay between PC and device. Too many lost packages.");
        return (int)BrainFlowExitCodes::BOARD_NOT_READY_ERROR;
    }
    time_delay =
        times.empty () ? 0.0 : std::accumulate (times.begin (), times.end (), 0.0) / times.size ();
    time_delay /= 2000; // 2 to get a half and 1000 to convert to secs
    safe_logger (spdlog::level::debug, "Time delta: {} seconds", time_delay);
    return (int)BrainFlowExitCodes::STATUS_OK;
}