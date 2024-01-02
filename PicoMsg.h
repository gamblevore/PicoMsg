
// picomsg
// Simple parent-child message-passing.

#ifndef __PICO_MSG__
#define __PICO_MSG__

#define PicoSilent			0
#define PicoNoisyChild		1
#define PicoNoisyParent		2
#define PicoNoisy			3

struct			PicoComms;
// i guess we can free old sends... on get/send
struct			PicoMessage          { char* Data;  int Length;  int NoAutoFree;  operator bool () {return Data;} };
struct			PicoDataGram         { char* Data;  int Length;  int Remain;  operator bool ()   {return Data;} };
struct 			PicoMessageConfig    { const char* Name;  int LargestMsg;  int Noise; int TotalReceived; int TotalSent; };

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

struct PicoThread {
	pthread_t 		Thr;
	atomic_int		Version;
} PicoThread1, PicoThread2;


static void pico_sleep (float Secs) {
	struct timespec ts = {0, (int)(1000000000.0f*Secs)};
	while (nanosleep(&ts, &ts)==-1 and  (errno == EINTR));
}

typedef int64_t PicoDate; // 20 bits for small stuff
PicoDate PicoGetDate () {
	timespec Now;
	clock_gettime(CLOCK_REALTIME, &Now);
	int64_t r = Now.tv_nsec*4398;
	r >>= 22;
	return r + (Now.tv_nsec << 20);
}




struct PicoCommsData : PicoMessageConfig {
// system	
	pollfd				Poller;
	short				Index;
	short				Socket;
	unsigned char		Err;
	bool				IsParent;

// msgs
	PicoDataGram		ReadingMsg;	
	PicoDataGram		SendingMsg;	
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
	atomic_int			Version;
	PicoComms*			Items[PicoMaxConnections+1]; // last item is always 0
	
	int AddCom (PicoComms* M) {
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

static int PicoOverHead(int N) {return N + sizeof(PicoMessage) + 16;};
struct PicoQueue : PicoTrousers { // We have to move this code DOWN to get away from the spiders! 😠
	deque<PicoDataGram>	Items;
	atomic_int			Limit;
	int					RunningTotal;
	int					BlockedAlerts;
	
	PicoQueue () {Limit=0; RunningTotal=0; BlockedAlerts=0;}
		
	PicoDataGram Get () {
		lock();
		PicoDataGram Msg = {};
		if (!Items.empty()) {
			Msg = Items.front();
			Items.pop_front();
			Limit += PicoOverHead(Msg);
		}
		unlock();
		return Msg;
	}
	
	bool Reserve (int N) {
		int L = Limit - PicoOverHead(N);
		if (L < 0) return false;
		Limit = L;
		return true;
	}
	
	void Append (PicoDataGram& Msg) {
		lock();
		RunningTotal++;
 		Items.push_back(Msg);
		unlock();
	}
	
	void Clear () { 
		for (auto& V: Items) 
			free(V.Data);
		Items.clear();
	}
}; 



static char CloseMsg[5] = {-1,-1,-1,-1, 0};

struct PicoComms : PicoCommsData {
	PicoQueue			Gotten;
	PicoQueue			ToSend;
	PicoTrousers		Sending;
	PicoTrousers		Reading;
	PicoTrousers		Connected;			// allow for read after they disconnect
	PicoTrousers		DestroyMe;
	
	pid_t Fork () {
		int Socks[2]; pid_t pid;
		if (socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)  or  (pid = fork()) < 0)
			return !!failed() - errno;
		
		IsParent = pid!=0;
		Connected = true;
		
		Say("ForkedOK");

		Socket = Socks[IsParent];
		Poller.fd = Socket;
		Poller.events = POLLOUT;
		close(Socks[!IsParent]);
		
		if (!start_thread(PicoThread1) /*or start_thread((PicoThread2)*/) return -errno;
		
		Index = PicoList.AddCom(this);
		Say("Threaded");
		
		return pid; 
	}

	bool send_reserve (int n) {
		if (n <= 0) return false;
		if (!Connected) return Say("Socket Is Closed");
		if (ToSend.Reserve(n)) return true;
		return got_backed_up(ToSend, n, "send");
	}
	
	bool AskSend (PicoMessage M, bool Prefixed) {
		if (!M.Data) return false;
		if (!send_reserve(M.Length)) {
			if (!M.NoAutoFree)
				free(M.Data);
			return false;
		}
		
		if (M.NoAutoFree) { // have to copy... sadly. If you want faster then don't noautofree. (prefix is good too)
			char* D = (char*)malloc(M.Length);
			if (!D) {
				ToSend.Reserve(-M.Length);
				return failed(ENOBUFS);
			}
			memcpy(D, M.Data, M.Length);
			M.Data = D;
		}
		
		if (Prefixed) {
			*((int*)(M.Data)) = htonl(M.Length);
		} else {
			PicoDataGram M2 = {0, M.Length, 4};
			ToSend.Append(M2);
		}
		
		auto Ref = *((PicoDataGram*)(&M));
		ToSend.Append(Ref);
		return true;
	}

	PicoMessage Get (float T = 0.0) {
		PicoDataGram M = Gotten.Get();
		if (!M) return {};
		if (T > 0) {
			PicoDate Final = PicoGetDate() + T*(1024.0*1024.0);
			while (!M and PicoGetDate() < Final)
				M = Gotten.Get();
		}
		return {M.Data, M.Length};
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
	PicoComms* constructor (int noise) {
		(*(PicoCommsData*)this) = {};
		Noise = noise;
		Name = "";
		ToSend.Limit = 16*1024*1024;
		Gotten.Limit = 16*1024*1024;
		LargestMsg   = 1024*1024;
		return this;
	}
			
	bool start_thread (PicoThread &T) {
		if (T.Thr) return true;
		if (pthread_create(&T.Thr, 0, pico_worker, 0) or pthread_detach(T.Thr))
			return failed();
		return true;
	}

	bool safe_read (PicoDataGram& Msg, int Flags=0) {
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
		Msg = {};
		return failed();
	}
	
	void terminate () {
		SendingMsg.Data = &CloseMsg[0]; 
		SendingMsg.Length = 4;
		SendingMsg.Remain = 0;
		safe_send();
	}
	
	void* disconnect (const char* why, bool Terminate=false) {
		if (!Connected) return nullptr;
		if (!Err)
			Err = ENOTCONN;
		Connected = false;
		if (Terminate)
			terminate();
		return Say("Closed", why);
	}

	void destroy () {
		DestroyMe = true;
		disconnect("deleted");
		if (Socket) // clear stuck send/recv
			Socket = close(Socket)*0;
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
		
	void* failed (int err=errno) {
		Err = err;
		return disconnect(strerror(err));
	}
		
	bool got_backed_up(PicoQueue& Q, int N, const char* op) {
		if (Q.BlockedAlerts++ < 8)
			Say("Message Queue Full.", op);
		Q.BlockedAlerts = 0; 
		return false;
	}
	
	bool alloc_msg () {
		int R = ReadingMsg.Remain;
		if (R > 0) return true;
		
		PicoDataGram Header = {(char*)(&R), 0, 4};
		if (!safe_read(Header, MSG_PEEK) or Gotten.Reserve(R))
			return got_backed_up(Gotten, R, "read");

		recv(Socket, &R, 4, MSG_NOSIGNAL|MSG_DONTWAIT); // discard

		R = ntohl(R);
		if (R == -1)
			return disconnect("Graceful");
		
		if (R <= 0 or R > LargestMsg)
			return failed(EDOM);

		char* S = (char*)calloc(R+1, 1);
		if (!S)
			return failed(ENOBUFS);
		
		ReadingMsg = {S, 0, R};
		return true;
	}
	
	void slurp () {
		if (!Socket or !Reading.enter()) return;
		while ( alloc_msg() and safe_read(ReadingMsg) )
			Gotten.Append(ReadingMsg); // The final chance to read things before closing...
		
		if (!Connected and Socket)
			Socket = 0 & close(Socket);
		Reading.unlock();
	}

	bool safe_send () {
		if (SendingMsg.Remain <= 0)
			SendingMsg = ToSend.Get();
		
		while (Connected and SendingMsg.Remain > 0) {
			int n = SendingMsg.Remain;
			const char* data = SendingMsg.Data + SendingMsg.Length;
			int Sent = (int)send(Socket, data, n, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Sent > 0) {
				SendingMsg.Remain -= Sent;
				SendingMsg.Length += Sent;
				if (SendingMsg.Remain <= 0)					break;	// success
				data += Sent;
			} else {
				int e = errno;
				if (!Sent or (Sent < 0 and e==EAGAIN))		return false;	// would block
				if (Sent < 0 and e==EINTR)					continue;		// try again
				return failed();											// error
			}
		}
		
		free(SendingMsg.Data);
		SendingMsg = {};
		return true;
	}

	void shoot () {
		if (Connected and Sending.enter()) {
			while (safe_send());
			Sending.unlock();
		}
	}
};


static float work_on_comms () {
	if (!PicoList.Count) return 0.5;
	
	for (int i = 0; auto M = PicoList[i]; i++)
		M->clean();
	// 🧹
	
	for (int i = 0; auto M = PicoList[i]; i++) {
		M->slurp();
		M->shoot();
	}
	
	return 0.001;
}

static void* pico_worker (void* obj) {
	while (true) pico_sleep(work_on_comms());
}

#endif



/// C-API ///
extern "C" PicoComms* PicoMsgComms (int Noise=PicoNoisy)  _pico_code_ (
	if (PicoList.Count >= PicoMaxConnections)
		return nullptr;
	return (new PicoComms)->constructor(Noise);
)

extern "C" void PicoMsgDestroy (PicoComms* M) _pico_code_ (
	if (M) M->destroy();
)

extern "C" int PicoMsgFork (PicoComms* M) _pico_code_ (
	return M->Fork();
)

extern "C" int PicoMsgErr (PicoComms* M) _pico_code_ (
	if (M->Connected)
		return 0;
	return M->Err;
)

extern "C" void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
	return M->Say(A, B, Iter, true);
)

extern "C" bool PicoMsgSend (PicoComms* M, PicoMessage Msg, bool Prefixed=false) _pico_code_ (
	return M->AskSend(Msg, Prefixed);
)

extern "C" bool PicoMsgCSend (PicoComms* M, const char* Str) _pico_code_ (
	PicoMessage Msg = {(char*)Str}; Msg.Length = (int)strlen(Str)+1; Msg.NoAutoFree = 1;
	return M->AskSend(Msg, true);
)

extern "C" PicoMessage PicoMsgGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get(Time);
)

#endif
