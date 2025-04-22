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
#include <thread>
#include <chrono>
#include <udt.h>
#include "cc.h"
#include "test_util.h"

using namespace std;

#ifndef WIN32
void* recvdata(void*);
void* monitor(void* s);
#else
DWORD WINAPI recvdata(LPVOID);
DWORD WINAPI monitor(LPVOID);
#endif

int main(int argc, char* argv[])
{
   // Parse flags: --one-off, --duration <sec>, optional port
   bool oneOff = false;
   int durationSeconds = -1;
   int portArgIndex = -1;

   for (int i = 1; i < argc; ++i) {
      if (0 == strcmp(argv[i], "--one-off")) {
         oneOff = true;
      }
      else if (0 == strcmp(argv[i], "--duration")) {
         if (i+1 < argc) {
            durationSeconds = atoi(argv[++i]);
            if (durationSeconds <= 0) {
               cerr << "invalid duration: " << argv[i] << endl;
               return 0;
            }
         } else {
            cerr << "usage: appserver [--one-off] [--duration seconds] [server_port]" << endl;
            return 0;
         }
      }
      else {
         // assume it's the port
         portArgIndex = i;
      }
   }

   // Validate total args
   if (argc > 5 ||
       (argc >= 2 && portArgIndex == -1 && argc > (oneOff + (durationSeconds>0) * 2 + 1)))
   {
      cout << "usage: appserver [--one-off] [--duration seconds] [server_port]" << endl;
      return 0;
   }

   // UDT startup/shutdown helper
   UDTUpDown _udt_;

   // Determine listening port
   string service("9000");
   if (portArgIndex != -1)
      service = argv[portArgIndex];

   // Resolve address
   addrinfo hints, *res;
   memset(&hints, 0, sizeof(hints));
   hints.ai_flags    = AI_PASSIVE;
   hints.ai_family   = AF_INET;
   hints.ai_socktype = SOCK_STREAM;

   if (0 != getaddrinfo(NULL, service.c_str(), &hints, &res)) {
      //cout << "illegal port number or port is busy.\n" << endl;
      return 0;
   }

   // Create UDT socket
   UDTSOCKET serv = UDT::socket(res->ai_family, res->ai_socktype, res->ai_protocol);

   // UDT Options
   //UDT::setsockopt(serv, 0, UDT_CC, new CCCFactory<CUDPBlast>, sizeof(CCCFactory<CUDPBlast>));
   UDT::setsockopt(serv, 0, UDT_MSS, new int(1500), sizeof(int));
   UDT::setsockopt(serv, 0, UDT_RCVBUF, new int(10000000), sizeof(int));
   UDT::setsockopt(serv, 0, UDP_RCVBUF, new int(10000000), sizeof(int));

   if (UDT::ERROR == UDT::bind(serv, res->ai_addr, res->ai_addrlen)) {
      //cout << "bind: " << UDT::getlasterror().getErrorMessage() << endl;
      return 0;
   }

   freeaddrinfo(res);

   //cout << "server is ready at port: " << service << endl;

   if (UDT::ERROR == UDT::listen(serv, 10)) {
      //cout << "listen: " << UDT::getlasterror().getErrorMessage() << endl;
      return 0;
   }


   // Accept loop
   sockaddr_storage clientaddr;
   int addrlen = sizeof(clientaddr);
   UDTSOCKET recver;

   while (true)
   {
      recver = UDT::accept(serv, (sockaddr*)&clientaddr, &addrlen);
      if (UDT::INVALID_SOCK == recver) {
         //cout << "accept: " << UDT::getlasterror().getErrorMessage() << endl;
         break;
      }

      char clienthost[NI_MAXHOST], clientservice[NI_MAXSERV];
      getnameinfo((sockaddr*)&clientaddr, addrlen,
                  clienthost, sizeof(clienthost),
                  clientservice, sizeof(clientservice),
                  NI_NUMERICHOST|NI_NUMERICSERV);
      //cout << "new connection: " << clienthost << ":" << clientservice << endl;
         // If duration specified, spawn a watcher thread to exit when time is up
      if (durationSeconds > 0) {
         thread([durationSeconds, serv]() {
            // sleep for the given duration
            this_thread::sleep_for(chrono::seconds(durationSeconds));
            //cout << "Duration of " << durationSeconds << " seconds reached, shutting down server." << endl;
            UDT::close(serv);
            exit(0);
         }).detach();
      }

      #ifndef WIN32
         pthread_t rcvthread;
         pthread_create(&rcvthread, NULL, recvdata, new UDTSOCKET(recver));
         if (oneOff) {
            pthread_join(rcvthread, NULL);
            break;
         }
         pthread_detach(rcvthread);
      #else
         HANDLE h = CreateThread(NULL, 0, recvdata, new UDTSOCKET(recver), 0, NULL);
         if (oneOff) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
            break;
         }
         CloseHandle(h);
      #endif
   }

   UDT::close(serv);
   return 0;
}

#ifndef WIN32
void* recvdata(void* usocket)
#else
DWORD WINAPI recvdata(LPVOID usocket)
#endif
{
   UDTSOCKET recver = *(UDTSOCKET*)usocket;
   delete (UDTSOCKET*)usocket;
   pthread_create(new pthread_t, NULL, monitor, &recver);

   const int size = 100000000;
   char* data = new char[size];

   while (true)
   {
      int rsize = 0;
      while (rsize < size)
      {
         int rcv_size = 0, var_size = sizeof(int);
         UDT::getsockopt(recver, 0, UDT_RCVDATA, &rcv_size, &var_size);
         int rs = UDT::recv(recver, data + rsize, size - rsize, 0);
         if (UDT::ERROR == rs) {
            cout << "recv: " << UDT::getlasterror().getErrorMessage() << endl;
            break;
         }
         rsize += rs;
      }
      if (rsize < size)
         break;
   }

   delete[] data;
   UDT::close(recver);

   #ifndef WIN32
      return NULL;
   #else
      return 0;
   #endif
}

#ifndef WIN32
void* monitor(void* s)
#else
DWORD WINAPI monitor(LPVOID s)
#endif
{
   UDTSOCKET u = *(UDTSOCKET*)s;
   UDT::TRACEINFO perf;
   cout << "time,bandwidth,total_packets" << endl;
   while (true)
   {
      #ifndef WIN32
         sleep(1);
      #else
         Sleep(1000);
      #endif

      if (UDT::ERROR == UDT::perfmon(u, &perf))
      {
         cout << "perfmon: " << UDT::getlasterror().getErrorMessage() << endl;
         break;
      }

      cout 
         << perf.msTimeStamp / 1000.0 << ","
         << perf.mbpsRecvRate << ","
         << perf.pktRecvTotal 
         << 
      endl;
   }

   #ifndef WIN32
      return NULL;
   #else
      return 0;
   #endif
}
