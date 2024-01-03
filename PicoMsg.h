
// picomsg
// Simple parent-child queue-based message-passing.
// uses two threads, and is non-blocking

// #define PicoMaxConnections <Num> to allow how many <Num> max connections.


#ifndef __PICO_MSG__
#define __PICO_MSG__

#define PicoSilent			0
#define PicoNoisyChild		1
#define PicoNoisyParent		2
#define PicoNoisy			3

struct			PicoComms;
struct			PicoMessage          { char* Data;  int Length;  bool NoAutoFree;  operator bool () {return Data;} };
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

#ifndef PicoMaxConnections
	#define PicoMaxConnections 16
#endif


using std::deque;
using std::atomic_bool; using std::atomic_int;


struct PicoDataGram  { PicoMessage M; int Remain; };
struct PicoCommsBase {
	atomic_int			RefCount;
	int					Index;
	PicoMessageConfig	Conf;

	void Decr() {
		if (RefCount-- == 1) delete this;
	}
};


static void* pico_worker (void* obj);
static pthread_t PicoThreads[2];


typedef int64_t PicoDate; // 20 bits for small stuff
PicoDate PicoGetDate () {
	timespec Now; clock_gettime(CLOCK_REALTIME, &Now);
	int64_t r = Now.tv_nsec*4398;
	r >>= 22;
	return r | (Now.tv_nsec << 20);
}


static bool pico_burn_msg(PicoMessage& M) {
	if (!M.NoAutoFree and M.Data)
		free(M.Data);
	M = {};
	return false;
}


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
	PicoCommsBase*		Items[PicoMaxConnections+1]; // last item is always 0
	
	void AddCom (PicoCommsBase* M) {
		lock();
		M->RefCount++;
		int I = M->Index = Count++; 
		Items[I] = M;
		unlock();
	}
	
	void Remove (int i) {
		lock();
		auto P = Items[i]; 
		int n = Count - 1;
		Items[i] = Items[n];
		Items[n] = 0;
		Count = n;
		P->RefCount--;
		unlock();
	}
	
	;;;/*_*/;;;   // <-- a spider that ehlps you do async work
	PicoComms* operator [] (int i) {
		lock();
		auto P = Items[i];
		P->RefCount++;
		unlock();
		return (PicoComms*)P;
	}
	
} PicoList;
    ;;;/*_*/;;;  ;;;/*_*/;;;     ;;;/*_*/;;;   // more spiders



//  |
//  |
//  v

static int PicoOverHead(int N) {return N + sizeof(PicoMessage) + 16;};
struct PicoQueue { // We have to move this code DOWN to get away from the spiders! 😠
	PicoTrousers		Altering;
	PicoTrousers		IO;
	PicoTrousers		IsOpen;			// allow for read after they disconnect
	atomic_int			BlockedAlerts;
	atomic_int			Limit;
	PicoDataGram		Msg;	

	deque<PicoMessage>	Items;
	
	PicoQueue () {Limit=0; BlockedAlerts=0; IsOpen=true;}
		
	PicoMessage Get () {
		PicoMessage Msg = {};
		Altering.lock();
		if (!Items.empty()) {
			Msg = Items.front();
			Items.pop_front();
			Limit += PicoOverHead(Msg.Length);
		}
		Altering.unlock();
		return Msg;
	}
	
	bool Reserve (int N) {
		int L = Limit - PicoOverHead(N);
		if (L < 0) return false;
		Limit = L;
		return true;
	}
		
	void Append (PicoMessage& Msg) {
 		Items.push_back(Msg);
	}
	
	void Burn () {
		for (auto& V: Items) 
			pico_burn_msg(V);
	}
}; 



static char CloseMsg[5] = {-1,-1,-1,-1, 0};

struct PicoComms : PicoCommsBase {
	int					Socket;
	unsigned char		Err;
	bool				IsParent;

	PicoQueue			Reading;
	PicoQueue			Sending;
	
	PicoComms* Constructor (int noise) {
		Socket = 0; Err = 0; IsParent = 0; Conf.Name = "";
		Conf.Noise = noise;
		Sending.Limit = 16*1024*1024;
		Reading.Limit = 16*1024*1024;
		Conf.LargestMsg = 1024*1024;
		RefCount = 1;
		return this;
	}
//
	pid_t Fork () {
		int Socks[2] = {}; pid_t pid;
		if (socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)  or  (pid = fork()) < 0) {
			close(Socks[0]); close(Socks[1]);
			return !!failed() - errno;
		}
		
		IsParent = pid!=0;
		Socket = Socks[IsParent];
		close(Socks[!IsParent]);
		
		if (!start_thread(&PicoThreads[0]) /*or start_thread((PicoThread2)*/) return -errno;
		
		PicoList.AddCom(this);
		Say("Started");
		
		return pid; 
	}
	
	bool QueueSend (PicoMessage M, bool Prefixed) {
		if (!M.Data or !send_reserve(M.Length))
			return pico_burn_msg(M);
		
		if (M.NoAutoFree and !own_msg(M)) return false; // must copy... sadly.
		
		if (Prefixed)
			*((int*)(M.Data)) = htonl(M.Length-4);
		PicoMessage Length = {0, M.Length, 1};

		Sending.Altering.lock();
		if (!Prefixed)
			Sending.Append(Length);
		Sending.Append(M);
		Sending.Altering.unlock();
		return true;
	}

	PicoMessage Get (float T = 0.0) {
		PicoMessage M = Reading.Get();
		if (!M and T > 0) {
			PicoDate Final = PicoGetDate() + T*(1024.0*1024.0);
			for (int i = 0;  !M and i < 10000;    i++) {
				auto Curr = PicoGetDate();
				if (Curr > Final) break;
				M = Reading.Get();
			}
		}
		return M;
	}

	void* Say (const char* A, const char* B="", int Iter=0, bool Strong=false) {
		int F = PicoNoisyChild << IsParent;
		if (!(Conf.Noise&F) and !Strong) return nullptr;
		const char* S = IsParent?"Us":"Them";
		
		if (Iter)
			printf("%s.%s says: %s %s %i\n", S, Conf.Name, A, B, Iter);
		  else
			printf("%s.%s says: %s %s\n", S, Conf.Name, A, B);
		
		return nullptr;
	}
	

//// INTERNALS ////
	bool start_thread (pthread_t* T) {
		if (*T) return true;
		if (pthread_create(T, 0, pico_worker, 0) or pthread_detach(*T))
			return failed();
		return true;
	}

	bool safe_read (PicoDataGram& Msg, int Flags=0) {
		while (Msg.Remain > 0) {
			int Sent = (int)recv(Socket, Msg.M.Data, Msg.Remain, Flags|MSG_DONTWAIT|MSG_NOSIGNAL);
			if (Sent > 0) {
				Msg.Remain -= Sent;
				Msg.M.Length += Sent;
				if (Msg.Remain == 0)						return true;	// success
				Msg.M.Data += Sent;
			} else {
				int e = errno;
				if (!Sent or e == EAGAIN)					return false;	// no datas
				if (e != EINTR)								break;			// error
																			// try again now
			}
		}

		pico_burn_msg(Msg.M);
		return failed();
	}

	inline bool own_msg (PicoMessage& M) {
		char* D = (char*)malloc(M.Length);
		if (!D) {
			Sending.Reserve(-M.Length);
			return failed(ENOBUFS);
		}
		memcpy(D, M.Data, M.Length);
		M.Data = D;
		return true;
	}
	
	bool send_reserve (int n) {
		if (n <= 0)			 return false;
		if (!Sending.IsOpen) return Say("Socket Is Closed");
		return Sending.Reserve(n) or got_backed_up(Sending, n, "send");
	}

	void send_close () {
		Sending.Msg.M.Data = &CloseMsg[0]; 
		Sending.Msg.M.Length = 4;
		Sending.Msg.Remain = 0;
		safe_send();
	}
	
	void* disconnect (const char* Why, bool SendClose=true) {
		if (!Sending.IsOpen) return nullptr;
		if (!Err)
			Err = ENOTCONN;
		Sending.IsOpen = false;
		if (SendClose)
			send_close();
		return Say("Closed", Why);
	}

	void destroy () {
		Reading.IsOpen = false;
		disconnect("deleted");
		if (Socket) // clear stuck send/recv
			Socket = close(Socket)*0;
		if (Index >= 0)
			PicoList.Remove(Index);
		Index = -1;
		Decr();
	}
	
	~PicoComms () {
		Reading.Burn();
		Sending.Burn();
	}
			
	void* failed (int err=errno) {
		Err = err;
		return disconnect(strerror(err));
	}
		
	bool got_backed_up(PicoQueue& Q, int N, const char* op) {
		if (Q.BlockedAlerts++ < 8)
			Say("Message Queue Full", op);
		Q.BlockedAlerts = 0; 
		return false;
	}
	
	bool alloc_msg () {
		int R = Reading.Msg.Remain;
		if (R > 0) return true;
		
		PicoDataGram Header = {(char*)(&R), 0, 1, R};
		if (!safe_read(Header, MSG_PEEK) or Reading.Reserve(R))
			return got_backed_up(Reading, R, "read");

		recv(Socket, &R, 4, MSG_NOSIGNAL|MSG_DONTWAIT); // discard

		R = ntohl(R);
		if (R == -1)
			return disconnect("Graceful", false);
		
		if (R <= 0 or R > Conf.LargestMsg)
			return failed(EDOM);

		char* S = (char*)calloc(R+1, 1);
		if (!S)
			return failed(ENOBUFS);
		
		Reading.Msg.M = {S, 0, true};
		Reading.Msg.Remain = R;
		return true;
	}
	
	void slurp () {
		if (!Socket or !Reading.IO.enter()) return;
		while ( alloc_msg() and safe_read(Reading.Msg) ) {
			Reading.Altering.lock();
			Reading.Append(Reading.Msg.M); // The final chance to read things before closing...
			Reading.Altering.unlock();
		}
		
		if (!Sending.IsOpen and Socket)
			Socket = 0 & close(Socket);
		Reading.IO.unlock();
	}

	bool safe_send () {
		if (Sending.Msg.Remain <= 0) {
			Sending.Msg.M = Sending.Get();
			Sending.Msg.Remain = 0;
		}
		
		while (Sending.IsOpen and Sending.Msg.Remain > 0) {
			const char* data = Sending.Msg.M.Data + Sending.Msg.M.Length;
			int Sent = (int)send(Socket, data, Sending.Msg.Remain, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Sent > 0) {
				Sending.Msg.Remain   -= Sent;
				Sending.Msg.M.Length += Sent;
				if (Sending.Msg.Remain <= 0)				break;	// success
				data += Sent;
			} else {
				int e = errno;
				if (!Sent or (Sent < 0 and e==EAGAIN))		return false;	// would block
				if (Sent < 0 and e==EINTR)					continue;		// try again
				return failed();											// error
			}
		}
		
		return !pico_burn_msg(Sending.Msg.M);
	}

	void shoot () {
		if (!Sending.IsOpen or !Sending.IO.enter()) return;

		while (safe_send()) {;;}
		Sending.IO.unlock();
	}
	
	void work () {
		slurp(); shoot(); Decr();
	}
};


static PicoDate PicoLastActivity;
static int work_on_comms () {
	sleep(1000);
	for (int i = 0; auto M = PicoList[i]; i++)
		M->work();
	
	const float NS = 1000000000.0;
	const float mul = 1.0 / (1024 * 1024 * 1024);
	float S = (PicoGetDate() - PicoLastActivity) * mul;
	S = S * S * (1.0 / 20000.0);
	if (S < 0.001) return (int)(0.001*NS);
	if (S >= 1.0) return (int)(0.999*NS);
	return (int)(S*NS);
}

static void* pico_worker (void* T) {
	while (true) {
		timespec ts = {0, work_on_comms()};
		nanosleep(&ts, 0);
	}
}

#endif



/// C-API ///
extern "C" PicoComms* PicoMsgComms (int Noise=PicoNoisy)  _pico_code_ (
	if (PicoList.Count >= PicoMaxConnections)
		return nullptr;
	return (new PicoComms)->Constructor(Noise);
)

extern "C" void PicoMsgDestroy (PicoComms* M) _pico_code_ (
	if (M) M->destroy();
)

extern "C" int PicoMsgFork (PicoComms* M) _pico_code_ (
	return M->Fork();
)

extern "C" int PicoMsgErr (PicoComms* M) _pico_code_ (
	return M->Sending.IsOpen ? 0 : M->Err;
)

extern "C" void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0) _pico_code_ (
	return M->Say(A, B, Iter, true);
)

extern "C" bool PicoMsgSend (PicoComms* M, PicoMessage Msg, bool Prefixed=false) _pico_code_ (
	return M->QueueSend(Msg, Prefixed);
)

extern "C" bool PicoMsgCSend (PicoComms* M, const char* Str) _pico_code_ (
	PicoMessage Msg = {(char*)Str}; Msg.Length = (int)strlen(Str)+1; Msg.NoAutoFree = 1;
	return M->QueueSend(Msg, false);
)

extern "C" PicoMessage PicoMsgGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get(Time);
)

extern "C" PicoMessageConfig* PicoMsgConf (PicoComms* M) _pico_code_ (
	return &M->Conf;
)

#endif
