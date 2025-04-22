#ifndef WIN32
   #include <unistd.h>
   #include <cstdlib>
   #include <cstring>
   #include <netdb.h>
#else
   #include <winsock2.h>
   #include <ws2tcpip.h>
   #include <wspiapi.h>
#endif
#include <iostream>
#include <udt.h>
#include <signal.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#include "pcc.h"

using namespace std;

#ifndef WIN32
void* monitor(void*);
#else
DWORD WINAPI monitor(LPVOID);
#endif

static atomic<bool> stopRequested(false);

/* Added: configurable monitor interval (default 1 s) */
#ifndef WIN32
static int monitor_interval_us = 1000000; // microseconds
#else
static int monitor_interval_ms = 1000;    // milliseconds
#endif

double rate_sum = 0;
double rtt_sum = 0;
double avg_loss_rate = 0;
double base_loss = 0;
double base_sent = 0;
unsigned int iteration_count = 0;

PCC* cchandle = NULL;

double PCC::kAlpha(1);
double PCC::kBeta(10.8);
double PCC::kExponent(0.9);
bool PCC::kPolyUtility(false);
double PCC::kFactor(0.1);
double PCC::kStep(0.05);
double PCC::kLatencyCoefficient(0);
double PCC::kInitialBoundary(0);
double PCC::kBoundaryIncrement(0);

void intHandler(int)
{
    stopRequested = true;
}

int main(int argc, char* argv[])
{
    int durationSeconds = -1;
    double intervalSeconds = 1.0; // default
    vector<char*> parms;

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--duration")) {
            if (i + 1 < argc) {
                durationSeconds = atoi(argv[++i]);
                if (durationSeconds <= 0) {
                    cerr << "Invalid duration: " << argv[i] << endl;
                    return 0;
                }
            } else {
                cerr << "usage: " << argv[0]
                     << " [--duration seconds] [--interval seconds] server_ip server_port"
                        " [factor] [step] [alpha] [beta] [exponent] [poly_utility]"
                     << endl;
                return 0;
            }
        }
        else if (0 == strcmp(argv[i], "--interval")) {
            if (i + 1 < argc) {
                intervalSeconds = atof(argv[++i]);
                if (intervalSeconds <= 0) {
                    cerr << "Invalid interval: " << argv[i] << endl;
                    return 0;
                }
            } else {
                cerr << "Missing value for --interval" << endl;
                return 0;
            }
        }
        else {
            parms.push_back(argv[i]);
        }
    }

    if (parms.size() < 2 || 0 == atoi(parms[1])) {
        cout << "usage: " << argv[0]
             << " [--duration seconds] [--interval seconds] server_ip server_port"
                " [factor] [step] [alpha] [beta] [exponent] [poly_utility]"
             << endl;
        return 0;
    }

#ifndef WIN32
    monitor_interval_us = static_cast<int>(intervalSeconds * 1e6);
#else
    monitor_interval_ms = static_cast<int>(intervalSeconds * 1e3);
#endif

    signal(SIGINT, intHandler);

    const char* server_ip   = parms[0];
    const char* server_port = parms[1];

    double alpha = 1, beta = 10.8, exponent = 0.9;
    bool use_poly = true;
    double factor = 1.0, step = 0.05;
    double latency = 0;
    double initial_boundary = 0.05;
    double boundary_increment = 0.06;

    for (size_t i = 2; i < parms.size(); ++i) {
        double v = atof(parms[i]);
        switch (i) {
            case 2: latency = v; break;
            case 3: factor = v; break;
            case 4: step = v; break;
            case 5: initial_boundary = v; break;
            case 6: boundary_increment = v; break;
            case 7: alpha = v; break;
            case 8: beta = v; break;
            case 9: exponent = v; break;
            case 10: use_poly = (v != 0); break;
            default: break;
        }
    }
    PCC::set_utility_params(alpha, beta, exponent, use_poly,
                            factor, step, latency,
                            initial_boundary, boundary_increment);

    UDT::startup();

    struct addrinfo hints, *local, *peer;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags    = AI_PASSIVE;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (0 != getaddrinfo(NULL, "9000", &hints, &local)) {
        cout << "incorrect network address." << endl;
        return 0;
    }

    UDTSOCKET client = UDT::socket(local->ai_family, local->ai_socktype, local->ai_protocol);
    UDT::setsockopt(client, 0, UDT_CC, new CCCFactory<PCC>, sizeof(CCCFactory<PCC>));
    UDT::setsockopt(client, 0, UDT_MSS, new int(1500), sizeof(int));
    UDT::setsockopt(client, 0, UDT_RCVBUF, new int(10000000), sizeof(int));
    UDT::setsockopt(client, 0, UDP_RCVBUF, new int(10000000), sizeof(int));
#ifdef WIN32
    UDT::setsockopt(client, 0, UDT_MSS, new int(1052), sizeof(int));
#endif
    freeaddrinfo(local);

    if (0 != getaddrinfo(server_ip, server_port, &hints, &peer)) {
        cout << "incorrect server address: " << server_ip << ":" << server_port << endl;
        return 0;
    }
    if (UDT::ERROR == UDT::connect(client, peer->ai_addr, peer->ai_addrlen)) {
        cout << "connect: " << UDT::getlasterror().getErrorMessage() << endl;
        return 0;
    }
    freeaddrinfo(peer);

    int temp;
    UDT::getsockopt(client, 0, UDT_CC, &cchandle, &temp);

    #ifndef WIN32
       pthread_create(new pthread_t, NULL, monitor, &client);
    #else
       CreateThread(NULL, 0, monitor, &client, 0, NULL);
    #endif

    if (durationSeconds > 0) {
        thread([durationSeconds]() {
            this_thread::sleep_for(chrono::seconds(durationSeconds));
            exit(0);
        }).detach();
    }

    const int size = 100000;
    char* data = new char[size];

    for (int i = 0; i < 1000000 && !stopRequested; ++i) {
        int sent = 0;
        while (sent < size && !stopRequested) {
            int ss;
            if (UDT::ERROR == (ss = UDT::send(client, data + sent, size - sent, 0))) {
                cout << "send: " << UDT::getlasterror().getErrorMessage() << endl;
                break;
            }
            sent += ss;
        }
        if (sent < size || stopRequested) break;
    }

    delete[] data;
    UDT::close(client);
    UDT::cleanup();
    return 0;
}

#ifndef WIN32
void* monitor(void* s)
#else
DWORD WINAPI monitor(LPVOID s)
#endif
{
    UDTSOCKET u = *(UDTSOCKET*)s;
    UDT::TRACEINFO perf;
    cout << "time,bandwidth,srtt,cwnd,total_packets,retr,lost" << endl;
    unsigned int i = 0;
    while (!stopRequested) {
        #ifndef WIN32
           usleep(monitor_interval_us);
        #else
           Sleep(monitor_interval_ms);
        #endif
        if (++i > 10000) break;
        if (UDT::ERROR == UDT::perfmon(u, &perf)) {
            cout << "perfmon: " << UDT::getlasterror().getErrorMessage() << endl;
            break;
        }
        cout 
            << perf.msTimeStamp / 1000.0    << ","
            << perf.mbpsSendRate            << ","
            << perf.msRTT                   << "," 
            << perf.pktCongestionWindow     << ","
            << perf.pktSent                 << ","  
            << perf.pktRetrans              << ","  
            << perf.pktSndLoss           << endl;
    }
#ifndef WIN32
    return NULL;
#else
    return 0;
#endif
}
