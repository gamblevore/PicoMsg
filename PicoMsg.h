
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
	#include <algorithm>

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


typedef int64_t PicoDate; // 16 bits for small stuff
PicoDate pico_date_create ( uint64_t S, uint64_t NS ) {
    NS /= (uint64_t)15259; // for some reason unless we spell this out, xcode will miscompile this.
    S <<= 16;
    return S + NS;
}
PicoDate PicoGetDate( ) {
	timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	return pico_date_create(ts.tv_sec, ts.tv_nsec);
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
		if (P) P->RefCount++;
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
	const char*			Name;

	deque<PicoMessage>	Items;
	
	PicoQueue () {Limit=0; BlockedAlerts=0; IsOpen=true; Msg = {}; Name = ""; }
	~PicoQueue () {
		pico_burn_msg(Msg.M);
		for (auto& V: Items) 
			pico_burn_msg(V);
	}
	
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
	
	void Append (PicoMessage& Msg) {
 		Items.push_back(Msg);
	}
}; 



static char CloseMsg[5] = {-1,-1,-1,-1, 0};

struct PicoComms : PicoCommsBase {
	int					Socket;
	unsigned char		Err;
	bool				IsParent;
	int					LengthBuff;

	PicoQueue			Reading;
	PicoQueue			Sending;
	
	PicoComms (int noise, bool isparent) {
		Socket = 0; Err = 0; IsParent = isparent; Conf.Name = "";
		Conf.Noise = noise;
		Sending.Limit = 16*1024*1024; Sending.Name = "send";
		Reading.Limit = 16*1024*1024; Reading.Name = "read";
		Conf.LargestMsg = 1024*1024;
		RefCount = 1;
	}
//
	PicoComms* Pair () {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return 0;
		PicoComms* Rz = new PicoComms(Conf.Noise, false);
		add_conn(Socks[0]);
		Rz->add_conn(Socks[1]);
		return Rz;
	}

	pid_t Fork () {
		int Socks[2] = {};
		if (!get_pair_of(Socks)) return -errno;
		pid_t pid = fork();
		if (pid < 0) return -errno;

		IsParent = pid!=0;
		close(Socks[!IsParent]);
		add_conn(Socks[IsParent]);
		return pid; 
	}
	
	bool QueueSend (PicoMessage M, bool Prefixed) {
		if (!M.Data or !own_msg(M) or !send_reserve(M.Length) )
			return pico_burn_msg(M);
		
		if (Prefixed)
			*((int*)(M.Data)) = htonl(M.Length-4);
		PicoMessage Length = {(char*)&LengthBuff, 4, 1};

		Sending.Altering.lock();
		if (!Prefixed)
			Sending.Append(Length);
		Sending.Append(M);
		Sending.Altering.unlock();
		return true;
	}

	PicoMessage Get (float T = 0.0) {
		PicoMessage M = Reading.Get();
		if (M or T <= 0) return M;
		PicoDate Final = PicoGetDate() + (PicoDate)(T*65536.0f);
		timespec ts = {0, 1000000};
		int n = T*16000; int i = 0;
		for (;  !M and i < n;    i++) {
			nanosleep(&ts, 0);
			PicoDate Curr = PicoGetDate();
			if (Curr > Final) break;
			M = Reading.Get();
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

	bool get_pair_of (int* P) {
		if (!socketpair(PF_LOCAL, SOCK_STREAM, 0, P)) return true;
		puts("closed!!");
		return failed();
	}
	
	void add_conn (int Sock) {
		Socket = Sock;
		PicoList.AddCom(this);
		Say("Started");
	}
	
	bool safe_read (PicoDataGram& Msg, int Flags=0) {
		while (Msg.Remain > 0) {
			int Got = (int)recv(Socket, Msg.M.Data, Msg.Remain, Flags|MSG_DONTWAIT|MSG_NOSIGNAL);
			if (Got > 0) {
				Msg.Remain -= Got;
				Msg.M.Length += Got;
				if (Msg.Remain == 0)						return true;	// success
				Msg.M.Data += Got;
			} else {
				int e = errno;
				if (!Got or e == EAGAIN)					return false;	// no datas
				if (e != EINTR)								break;			// error
																			// try again now
			}
		}

		pico_burn_msg(Msg.M);
		return failed();
	}

	bool queue_reserve (PicoQueue& Q, int N) {
		int L = Q.Limit - PicoOverHead(N);
		if (L >= 0) {Q.Limit = L; return true;};

		if (Q.BlockedAlerts++ < 8)
			Say("Message Queue Full", Q.Name);
		Q.BlockedAlerts = 0; 
		return false;
	}
		
	inline bool own_msg (PicoMessage& M) {
		if (!M.NoAutoFree) return true;
		char* D = (char*)malloc(M.Length);
		if (!D)
			return failed(ENOBUFS);
		memcpy(D, M.Data, M.Length);
		M.Data = D;
		M.NoAutoFree = 0;
		return true;
	}
	
	bool send_reserve (int n) {
		if (n <= 0)			 return false;
		if (!Sending.IsOpen) return Say("Socket Is Closed");
		return queue_reserve(Sending, n);
	}

	void send_close () {
		Sending.Msg.M.Data = &CloseMsg[0]; 
		Sending.Msg.M.Length = 4;
		Sending.Msg.Remain = 0;
		safe_send();
	}
	
	void* disconnect (const char* Why, bool SendClose=true) {
		pico_burn_msg(Sending.Msg.M);
		pico_burn_msg(Reading.Msg.M);
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
		puts("destroyed!!");
		if (Socket) // clear stuck send/recv
			Socket = close(Socket)*0;
		if (Index >= 0)
			PicoList.Remove(Index);
		Index = -1;
		Decr();
	}
			
	void* failed (int err=errno) {
		Err = err;
		return disconnect(strerror(err));
	}
		
	bool alloc_msg () {
		int R = Reading.Msg.Remain;
		if (R > 0) return true;
		
		PicoDataGram Header = {(char*)(&R), 0, true, 4};
		if (!safe_read(Header, MSG_PEEK)) return false;
		
		R = ntohl(R);
		Say("Read", "", R);
		char Garbage[4]; auto G = recv(Socket, Garbage, 4, MSG_NOSIGNAL|MSG_DONTWAIT); // discard
		Say("Grbg", "", (int)G);
		if (R == -1)
			return disconnect("Graceful", false);
		
		if (!queue_reserve(Reading, R))
			return false;
		if (R <= 0 or R > Conf.LargestMsg)
			return failed(EDOM);

		char* S = (char*)malloc(R+1);
		if (!S)
			return failed(ENOBUFS);
		S[R] = 0;
		
		Reading.Msg.M = {S};
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
		
		if (!Sending.IsOpen and Socket) // deliberately on Sending, not Reading here. By design.
			Socket = 0 & close(Socket) & puts("slurp died!!");
		Reading.IO.unlock();
	}

	bool safe_send () {
		auto& M = Sending.Msg;
		if (M.Remain <= 0) {
			M.M = Sending.Get();
			if (!M.M.Data) return false;
			M.Remain = M.M.Length;
			M.M.Length = 0;
			LengthBuff = htonl(M.Remain);
//			M.M = M.M;
		}
		
		while (Sending.IsOpen and M.Remain > 0) {
			const char* data = M.M.Data + M.M.Length;
			int Sent = (int)send(Socket, data, M.Remain, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Sent > 0) {
				M.Remain   -= Sent;
				M.M.Length += Sent;
				if (M.Remain <= 0)							break;	// success
				data += Sent;
			} else {
				int e = errno;
				if (!Sent or (Sent < 0 and e==EAGAIN))		return false;	// would block
				if (Sent < 0 and e==EINTR)					continue;		// try again
				return failed(e);											// error
			}
		}
		
		return !pico_burn_msg(M.M);
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
static float work_on_comms () {
	for (int i = 0; auto M = PicoList[i]; i++)
		M->work();
	
	float S = (PicoGetDate() - PicoLastActivity) * 0.000015258789f;
	return std::clamp(S * S * 0.00005f, 0.001f, 0.999f) * 1000000000.0f;
}

static void* pico_worker (void* T) {
	while (true) {
		timespec ts = {0, (int)work_on_comms()};
		nanosleep(&ts, 0);
	}
}

bool pico_start () {
	if (PicoList.Count >= PicoMaxConnections)
		return puts("Pico: Too many threads.")*0;
	if (PicoThreads[0]) return true;
	for (int i = 0; i < 1; i++) {
		auto T = &PicoThreads[i];
		if (pthread_create(T, 0, pico_worker, 0) or pthread_detach(*T))
			return puts("Pico: Unable to create threads.")*0;
	}
	return true;
}

#endif



/// C-API ///
extern "C" PicoComms* PicoMsgComms (int Noise=PicoNoisy)  _pico_code_ (
	if (!pico_start()) return nullptr;
	return new PicoComms(Noise, true);
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

extern "C" bool PicoMsgSendStr (PicoComms* M, const char* Str) _pico_code_ (
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
