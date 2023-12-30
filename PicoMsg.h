
// picomsg
// Simple parent-child IPC message-passing.

#ifndef __PICO_MSG__
#define __PICO_MSG__

#define PicoNoisyChild 1
#define PicoNoisyParent 2
#define PicoNoisy 3

struct			PicoComms;
struct			PicoMessage			{ int Remain;  int Length;  char* Data;  operator bool () {return Data;} };
struct 			PicoMessageConfig	{ const char* Name;  int MaxSend;  int Flags; };

#ifndef PICO_IMPLEMENTATION
	#define _pico_code_(x) ;
#else
	#define _pico_code_(x) {x}

	#include <poll.h>
	#include <fcntl.h>
	#include <stdio.h>
	#include <unistd.h>
	#include <string>
	#include <errno.h>
	#include <sys/socket.h>
	#include <csignal>
	#include <atomic>
	#include <deque>

#ifndef PicoMaxConnections // can define this yourself before including this file!
	#define PicoMaxConnections 16
#endif


using std::deque;
using std::atomic_bool; using std::atomic_int;


typedef void*   (*fpThread)		(void* Obj);
static void*	pico_worker		(void* obj);


static void chill_a_bit () {
	struct timespec ts = {0, 1000000};
	nanosleep(&ts, 0);
}

struct PicoSection {
	atomic_bool Value;
	bool use () {
		bool Expected = false;
		return Value.compare_exchange_weak(Expected, true);
	}
	void finish () {
		Value = false;
	}
	void lock () {
		while (!use()); // spin
	}
};


struct PicoList {
	atomic_int			Count;
	PicoSection			Lock;
	PicoComms*			Items[PicoMaxConnections+1]; // last item is always 0
	
	int Append (PicoComms* M) {
		Lock.lock();
		int rz = Count++;
		Items[rz] = M;
		Lock.finish();
		return rz;
	}
	
	void Remove (int i) {
		Lock.lock();
		int n = Count - 1;
		Items[i] = Items[n];
		Items[n] = 0;
		Count = n;
		Lock.finish();
	}
	
	;;;/*_*/;;;   // <-- a spider that ehlps you do async work
	PicoComms* operator [] (int i) {
		return Items[i];
	}
	
} PicoList;
    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // more spiders


static pthread_t					ReadThread;
static pthread_t					SendThread;
uint								MessagesReceived;


struct PicoCommsData : PicoMessageConfig {
// system	
	int					Index;
	int					Socket;
	int					Err;
	pollfd				Poller;
	bool				IsParent;

// msgs
	deque<PicoMessage>	Gotten;
	deque<PicoMessage>	ToSend;
	uint				TotalSent;
	PicoMessage			ReadMsg;	
};


struct PicoComms : PicoCommsData {
	PicoSection			Lock;				// wont compile otherwise.
	atomic_bool			WantRemove;
	atomic_bool			CanSend;			// allow for read after they disconnect
	
	pid_t Fork () {
		int Socks[2];
		Err = (char)socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks);
		if (Err) {
			failed();
			return -1;
		}

		pid_t pid = fork();
		if (pid < 0) {
			Err = errno;
			disconnect("fork");
			return -1;
		}
		
		IsParent = pid!=0;
		CanSend = true;
		
		Say("ForkedOK");

		Socket = Socks[IsParent];
		Poller.fd = Socket;
		Poller.events = POLLOUT;
		close(Socks[!IsParent]);
		
		if (!start_thread()) return false;
		
		Index = PicoList.Append(this);
		Say("Threaded");
		
		return pid; 
	}

	PicoMessage Get (int Attempts) {
		while (Attempts --> 0) {
			auto Msg = Get();
			if (Msg.Data)
				return Msg;
			chill_a_bit();
		}
		return {};
	}
	
	bool Send (const void* data, int n=-1) {
		if (n == -1)
			n = (int)strlen((const char*)data) + 1;
		if (n <= 0) return true; // whatever
//		Say("sent", "", TotalSent);
		int n2 = htonl(n);
		return safe_send(&n2, 4) and safe_send(data, n);
	}

	PicoMessage Get () {
		if (Gotten.empty())
			return {};
		Lock.lock();
		auto Msg = Gotten.front(); Gotten.pop_front();
		Lock.finish();
		return Msg;
	}

	void* Say (const char* A, const char* B="", int Iter=0, bool Strong=false) {
		int Noise = PicoNoisyChild << IsParent;
		if (!(Flags&Noise) and !Strong) return nullptr;
		const char* S = IsParent?"Us":"Them";
		
		if (Iter)
			printf("%s.%s says: %s %s %i\n", S, Name, A, B, Iter);
		  else
			printf("%s.%s says: %s %s\n", S, Name, A, B);
		
		return nullptr;
	}
	

//// INTERNALS
	void* disconnect (const char* why, bool Terminate=false) {
		if (!CanSend) return nullptr;
		CanSend = false;
		if (Terminate) {
			int CloseMsg = -1;
			safe_send(&CloseMsg, 4);
		}
		return Say("Closed", why);
	}

	PicoComms* constructor (int flags) {
		(*(PicoCommsData*)this) = {};
		WantRemove = false;
		Lock.Value = false;
		Flags = flags;
		Name = "";
		MaxSend = 1024*1024;
		return this;
	}
	
	bool can_send () {
		if (!CanSend) return Say("Socket Is Closed");
		int Amount = poll(&Poller, 1, 0);
		return Amount > 0;
	}
		
	bool thread (pthread_t* t, fpThread R, void* Mode) {
		if (pthread_create(t, 0, R, Mode))
			return failed();
		if (pthread_detach(*t))
			return failed();
		return true;
	}
	
	bool start_thread () {
		auto F = (void*)-1;
		return thread(&ReadThread, pico_worker, F)  and  thread(&SendThread, pico_worker, 0);
	}

	bool safe_read (PicoMessage& Msg, int Flags=0) {
		while (Msg.Remain > 0) {
			int Sent = (int)recv(Socket, Msg.Data, Msg.Remain, Flags|MSG_DONTWAIT|MSG_NOSIGNAL);
			if (Sent > 0) {
				Msg.Remain -= Sent;
				Msg.Length += Sent;
				if (Msg.Remain == 0)						return true;	// success
				Msg.Data += Sent;
			} else {
				int e = errno;
				if (!Sent or e == EAGAIN)					return false;	// no datas
				if (e != EINTR)								break;			// error
																			// try again now
			}
		}

		free(Msg.Data);
		Msg.Data = 0;
		Msg.Length = -1;
		return failed();
	}
		
	void* failed (int err=errno) {
		Err = err;
		return disconnect(strerror(err));
	}
	
	bool safe_send (const void* data_, int n) {
		if (!can_send()) return false;

		const char* data = (const char*)(data_); // debug
		while (n > 0) {
			int Sent = (int)send(Socket, data, n, MSG_NOSIGNAL);
			if (Sent > 0) {
				TotalSent += Sent;
				if (n == Sent)										return true;	// success
				n -= Sent;
				data += Sent;
			} else {
				int e = errno;
				if (Sent < 0 and (e==EINTR or e==EAGAIN))			continue;		// try again
				if (Sent <= 0)										break;			// error
			}
		}
		return failed();
	}
	
	bool alloc_msg () {
		int R = ReadMsg.Remain;
		if (R > 0) return true;
		
		PicoMessage Header = {4, 0, (char*)(&R)};
		if (!safe_read(Header, MSG_PEEK))
			return false;

		recv(Socket, &R, 4, MSG_NOSIGNAL|MSG_DONTWAIT); // discard

		R = ntohl(R);
		if (R == -1)
			return disconnect("Graceful");
		
		if (R <= 0 or R > MaxSend)
			return failed(EDOM);

		char* S = (char*)calloc(R+1, 1);
		if (!S)
			return failed(ENOBUFS);
		
		ReadMsg = {R, 0, S};
		return true;
	}

	bool append_msg () {
		Lock.lock();
		Gotten.push_back(ReadMsg);
		Lock.finish();
		MessagesReceived++;
		return true;
	}
	
	void try_close() {
		if (!CanSend and Socket) {
			close(Socket);
			Socket = 0;
		}
	}
	
	void destroy () {
		PicoList.Remove(Index);
		disconnect("delete");
		for (auto V: Gotten) {
			free(V.Data);
		}
		try_close();
		delete this;
	}
	
	void read () {
		if (WantRemove)
			return destroy();
		while ( alloc_msg()
			and safe_read(ReadMsg)
			and append_msg()
		);
		// The final chance to read things before closing...
		try_close();
	}
		
	void write() {
		
	}
};


static void pico_sleep (float Secs) {
	struct timespec ts = {0, (int)(1000000000.0f*Secs)};
	while (nanosleep(&ts, &ts)==-1 and  (errno == EINTR));
}


static void pico_msg_work (bool Read) {
	for (int i = 0; auto M = PicoList[i]; i++) {
		Read ? M->read() : M->write();
	}
}

static void* pico_worker (void* obj) {
	while (true) {
		if (PicoList.Count) {
			pico_msg_work(obj);
			pico_sleep(0.01);
		} else {
			pico_sleep(0.5);
		}
	}
	return nullptr;
}

#endif



/// C-API ///
PicoComms* PicoMsg (int Flags=PicoNoisy)  _pico_code_ (
	if (PicoList.Count >= PicoMaxConnections)
		return nullptr;
	return (new PicoComms)->constructor(Flags);
)

void PicoMsgDestroy (PicoComms* M) _pico_code_ (
	if (M) M->WantRemove = true;
)

int PicoMsgFork (PicoComms* M) _pico_code_ (
	return M->Fork();
)

PicoMessage PicoMsgGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get();
)

bool PicoMsgSend (PicoComms* M, const void* data, int n=-1) _pico_code_ (
	return M->Send(data, n);
)

int PicoMsgErr (PicoComms* M) _pico_code_ (
	if (M->CanSend)
		return 0;
	if (!M->Err) // err is really just for user-diagnostic
		M->Err = ENOTCONN;
	return M->Err;
)

void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0, bool Strong=true) _pico_code_ (
	return M->Say(A, B, Iter, Strong);
)

PicoMessageConfig* PicoMsgConfig (PicoComms* M) _pico_code_ (
	return M;
)

#endif
