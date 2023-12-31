
// picomsg
// Simple parent-child message-passing.

#ifndef __PICO_MSG__
#define __PICO_MSG__

#define PicoSilent 0
#define PicoNoisyChild 1
#define PicoNoisyParent 2
#define PicoNoisy 3

struct			PicoComms;
// i guess we can free old sends... on get/send
struct			PicoMessage			{ char* Data; int Length; int Remain; void* Owner; operator bool () {return Data;} };
struct 			PicoMessageConfig	{ const char* Name;  int LargestMsg;  int Noise; int TotalReceived; int TotalSent; };

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

static pthread_t					Thread;
uint								MessagesReceived;


struct PicoCommsData : PicoMessageConfig {
// system	
	short				Index;
	short				Socket;
	unsigned char		Err;
	bool				IsParent;
	pollfd				Poller;

// msgs
	uint				TotalSent;
	PicoMessage			ReadMsg;	
};


struct PicoTrousers { // only one person can wear them at a time.
	atomic_bool Value;
	PicoTrousers  () {Value = false;}
	operator bool () {return Value;}
	void operator = (bool b) {Value = b;}
	bool enter () {
		bool Expected = false;
		return Value.compare_exchange_weak(Expected, true);
	}
	void unlock () {
		Value = false;
	}
	void lock () {
		while (!enter()); // spin
	}
};


struct PicoCommList : PicoTrousers {
	atomic_int			Count;
	PicoComms*			Items[PicoMaxConnections+1]; // last item is always 0
	
	int Append (PicoComms* M) {
		lock();
		int rz = Count++;
		Items[rz] = M;
		unlock();
		return rz;
	}
	
	void Remove (int i) {
		lock();
		int n = Count - 1;
		Items[i] = Items[n];
		Items[n] = 0;
		Count = n;
		unlock();
	}
	
	;;;/*_*/;;;   // <-- a spider that ehlps you do async work
	PicoComms* operator [] (int i) {
		return Items[i];
	}
	
} PicoList;
    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // more spiders



//  |
//  |
//  v
struct PicoQueue : PicoTrousers { // We have to move this code DOWN to get away from the spiders! 😠
	deque<PicoMessage>	Items;
	
	PicoMessage Get () {
		if (Items.empty())
			return {};
		lock();
		auto Msg = Items.front(); Items.pop_front();
		unlock();
		return Msg;
	}
	
	void Append (PicoMessage& Msg) {
		lock();
 		Items.push_back(Msg);
		unlock();
	}
	
	void Clear () {
		for (auto& V: Items) 
			free(V.Data);
		Items.clear();
	}
}; 


static void chill_a_bit () {
	struct timespec ts = {0, 1000000};
	nanosleep(&ts, 0);
}


struct PicoComms : PicoCommsData {
	PicoQueue			Gotten;
	PicoQueue			ToSend;
	PicoTrousers		DestroyMe;
	PicoTrousers		Sending;
	PicoTrousers		Reading;
	PicoTrousers		Connected;			// allow for read after they disconnect
	
	pid_t Fork () {
		int Socks[2];
		if (socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks))
			return -!failed();

		pid_t pid = fork();
		if (pid < 0)
			return -!failed();
		
		IsParent = pid!=0;
		Connected = true;
		
		Say("ForkedOK");

		Socket = Socks[IsParent];
		Poller.fd = Socket;
		Poller.events = POLLOUT;
		close(Socks[!IsParent]);
		
		if (!Thread and !start_thread()) return false;
		
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
		return Gotten.Get();
	}

	void* Say (const char* A, const char* B="", int Iter=0, bool Strong=false) {
		int F = PicoNoisyChild << IsParent;
		if (!(Noise&F) and !Strong) return nullptr;
		const char* S = IsParent?"Us":"Them";
		
		if (Iter)
			printf("%s.%s says: %s %s %i\n", S, Name, A, B, Iter);
		  else
			printf("%s.%s says: %s %s\n", S, Name, A, B);
		
		return nullptr;
	}
	

//// INTERNALS
	PicoComms* constructor (int flags) {
		(*(PicoCommsData*)this) = {};
		Noise = flags;
		Name = "";
		LargestMsg = 1024*1024;
		return this;
	}
	
	bool can_send () {
		if (!Connected) return Say("Socket Is Closed");
		int Amount = poll(&Poller, 1, 0);
		return Amount > 0;
	}
		
	bool start_thread () {
		if (pthread_create(&Thread, 0, pico_worker, 0) or pthread_detach(Thread))
			return failed();
		return true;
	}

	bool safe_read (PicoMessage& Msg, int Flags=0) {
		while (Msg.Remain > 0) {
			int Sent = (int)recv(Socket, Msg.Data, Msg.Remain, Flags|MSG_DONTWAIT|MSG_NOSIGNAL);
			if (Sent > 0) {
				Msg.Remain -= Sent;
				Msg.Length += Sent;
				if (Msg.Remain == 0)						return true;// success
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
	
	void* disconnect (const char* why, bool Terminate=false) {
		if (!Connected) return nullptr;
		Connected = false;
		if (Terminate) {
			int CloseMsg = -1;
			safe_send(&CloseMsg, 4);
		}
		return Say("Closed", why);
	}

	void destroy () {
		DestroyMe = true;
		disconnect("deleted");
		if (Socket) // clear stuck send/recv
			Socket = 0 & close(Socket);
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
		
		PicoMessage Header = {(char*)(&R), 0, 4};
		if (!safe_read(Header, MSG_PEEK))
			return false;

		recv(Socket, &R, 4, MSG_NOSIGNAL|MSG_DONTWAIT); // discard

		R = ntohl(R);
		if (R == -1)
			return disconnect("Graceful");
		
		if (R <= 0 or R > LargestMsg)
			return failed(EDOM);

		char* S = (char*)calloc(R+1, 1);
		if (!S)
			return failed(ENOBUFS);
		
		ReadMsg = {S, 0, R};
		return true;
	}
	
	void clean () {
		if (!DestroyMe) return;
		
		PicoList.Remove(Index);
		Reading.lock();
		Gotten.Clear();
		Sending.lock();
		ToSend.Clear();
		delete this;
	}
	
	void slurp () {
		if (!Socket or !Reading.enter()) return;
		while ( alloc_msg() and safe_read(ReadMsg) ) {
			TotalReceived++;
			Gotten.Append(ReadMsg);
		}		// The final chance to read things before closing...
		
		if (!Connected and Socket)
			Socket = 0 & close(Socket);
		Reading.unlock();
	}
		
	void shoot () {
		if (!Connected or !Sending.enter()) return;
		TotalSent++;
		Sending.unlock();
	}
};


static void pico_sleep (float Secs) {
	struct timespec ts = {0, (int)(1000000000.0f*Secs)};
	while (nanosleep(&ts, &ts)==-1 and  (errno == EINTR));
}


static float work_on_comms () {
	if (!PicoList.Count)
		return 0.5;
	
	for (int i = 0; auto M = PicoList[i]; i++)
		M->clean();     // 🧹
	
	for (int i = 0; auto M = PicoList[i]; i++) {
		M->slurp();     // 🥤
		M->shoot();     // 🏹 
	}
	
	return 0.001;
}

static void* pico_worker (void* obj) {
	while (true)
		pico_sleep(work_on_comms());
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
	if (M) M->destroy();
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
	if (M->Connected)
		return 0;
	if (!M->Err) // err is really just for user-diagnostic
		M->Err = ENOTCONN;
	return M->Err;
)

void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
	return M->Say(A, B, Iter, true);
)

#endif
