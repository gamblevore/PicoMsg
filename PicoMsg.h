
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
struct			PicoMessage        { char* Data;       int Length;  operator bool () {return Data;} };
struct 			PicoMessageConfig  { const char* Name; int LargestMsg; int Noise; int TotalReceived; int TotalSent; };

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


struct PicoCommsBase {
	atomic_int			RefCount;
	int					Index;
	PicoMessageConfig	Conf;

	void Decr() {
		if (RefCount-- == 1) delete this;
	}
};


static	void*			pico_worker (void* obj);
typedef int64_t			PicoDate;  // 16 bits for small stuff
typedef ssize_t			(*pico_io_fn)(int, void *, size_t, int);
static	pthread_t		PicoThreads[2];
static	PicoDate		PicoLastActivity;


PicoDate pico_date_create ( uint64_t S, uint64_t NS ) {
    NS /= (uint64_t)15259; // for some reason unless we spell this out, xcode will miscompile this.
    S <<= 16;
    return S + NS;
}

PicoDate PicoGetDate( ) {
	timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	return pico_date_create(ts.tv_sec, ts.tv_nsec);
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


struct PicoBuff { // We have to move this code DOWN to get away from the spiders! 😠
	const char*			Name;
	char*				SectionStart;
	int					Start;
	int					Length;
	int					Size;
	PicoTrousers		IO;
	PicoTrousers		IsOpen;			// allow for read after they disconnect
	
	PicoBuff () {IsOpen=true; Name = ""; SectionStart = 0; Start = 0; Length = 0; Size = 0;}
	
	bool Alloc (int n, const char* name) {
		Name = name;
		if (!(SectionStart = (char*)calloc(n, 1))) return false;
		Start = 0; Length = 0; Size = n;
		return true;
	}
	
/*
	||||||||------- // start
	----||||------- // middle
	------||||||||| // end (partial)
	||||||||||||||| // end (whole)
	|||||-----||||| // overlapping
*/
	
	
	// Sync:  .AskEmpty + .Lost   with   .Append
	PicoMessage AskEmpty () {
		auto L = Length;
		if (!L) return {};

		auto S = Start;
		auto From = SectionStart+S;
		return {From, std::min(S + L, Size)};
	}

	void Lost (int N) {
		PicoLastActivity = PicoGetDate();
		IO.lock();
		Start += N;
		auto L2 = Length - N;
		Length = L2;
		if (!L2 or Start >= Size)
			Start = 0;
		IO.unlock();
	}

	bool Append (const char* Src, int ML) {
		int L = htonl(ML);
		return AppendSub((char*)&L, 4) and AppendSub(Src, ML);
	}
	
	bool AppendSub (const char* Src, int Need) {
		while (auto Dest = AskFill()) {
			int Avail = std::min(Need, Dest.Length);
			Need -= Avail;
			Gained(Avail);
			memcpy(Dest.Data, Src, Avail);
			if (Need <= 0) return true;
		};
		return false;
	}
	
	/////// sync:  .AskFill + .Gained   with   .Get
	PicoMessage AskFill () {
		auto L = Length;
		auto S = Start;
		auto M = Size; 
		if (L >= M) return {};
		auto FromPos = L + S;
		if (FromPos >= M)
			FromPos -= M;
			M = S;
		
		return {SectionStart + FromPos, M - FromPos};
	}
	
	void Gained (int N) { 
		PicoLastActivity = PicoGetDate();
		IO.lock();
		Length += N; // seems fair?
		IO.unlock();
	}

	bool Get (char* Dest, int N) {
		if (Length < N)
			return false;
		if (Dest) while (auto Msg = AskEmpty()) {
			int B = std::min(Msg.Length, N);
			memcpy(Dest, Msg.Data, B);
			Dest += B;
			N -= B;
			Lost(B);
		}
		return true;
	}
}; 



static char PicoCloseData[4] = {-1,-1,-1,-1};

struct PicoComms : PicoCommsBase {
	int					Socket;
	unsigned char		Err;
	bool				IsParent;
	int					LengthBuff;

	PicoBuff			Reading;
	PicoBuff			Sending;
	
	PicoComms (int noise, bool isparent) {
		RefCount = 1; Socket = 0; Err = 0; IsParent = isparent; Conf.Name = isparent?"Parent":"Child";
		Conf.Noise = noise;
		if (!Sending.Alloc(1024*1024, "send") or !Reading.Alloc(1024*1024, "read"))
			failed(ENOBUFS);
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
	
	bool QueueSend (const char* msg, int n) {
		return Sending.Append(msg, n) or failed(ENOBUFS);
	}

	PicoMessage Get (float T = 0.0) {
		PicoMessage M = get_sub();
		if (M or T <= 0) return M;
		PicoDate Final = PicoGetDate() + (PicoDate)(T*65536.0f);
		timespec ts = {0, 1000000}; int n = T*16000; int i = 0;
		for (;  !M and i < n;    i++) {
			nanosleep(&ts, 0);
			if (PicoGetDate() > Final) break;
			M = get_sub();
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

	void do_reading () {
		while ( auto Msg = Reading.AskFill() ) {
			int Amount = (int)recv(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Amount > 0)
				Reading.Gained(Amount);
			  else if (!io_pass(Amount))
				break;
		}
		
		Reading.IO.unlock();
	}
	
	void do_sending () {
		while ( auto Msg = Sending.AskEmpty() ) {
			int Amount = (int)send(Socket, Msg.Data, Msg.Length, MSG_NOSIGNAL|MSG_DONTWAIT);
			if (Amount > 0)
				Sending.Lost(Amount);
			  else if (!io_pass(Amount))
				break;
		}
		
		Sending.IO.unlock();
	}
	
	PicoMessage get_sub () {
		int Length = 0;
		if (Reading.Get((char*)&Length, 4)) {
			if (Reading.Get(0, Length)) {
				char* Data = (char*)malloc(Length);
				if (Data) {
					Reading.Get(Data, Length);
					return {Data, Length};
				}
				failed(ENOBUFS);
			}
		}
		return {};
	}
	
	bool get_pair_of (int* Socks) {
		if (!socketpair(PF_LOCAL, SOCK_STREAM, 0, Socks)) return true;
		return (bool)failed() * puts("closed!!");
	}
	
	void add_conn (int Sock) {
		Socket = Sock;
		PicoList.AddCom(this);
		Say("Started");
	}
	
	void* disconnect (const char* Why, bool SendClose=true) {
		if (!Sending.IsOpen) return nullptr;
		if (!Err)
			Err = ENOTCONN;
		Sending.IsOpen = false;
		if (SendClose)
			QueueSend(PicoCloseData, 4);
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
		
	bool io_pass (int Amount) {
		if (!Amount) return false;
		int e = errno;
		if (e == EINTR)  return true;
		if (e == EAGAIN) return false;
		return failed(e);
	}
	
	void io () {
		if (Socket and Reading.IO.enter())
			do_reading();
		if (Sending.IsOpen and Sending.IO.enter())
			do_sending();
		  else if (Socket)
			Socket = 0 & close(Socket) & puts("io died!!");
		Decr(); 
	}
};


static void pico_work_comms () {
	for (int i = 0; auto M = PicoList[i]; i++)
		M->io();
	
	float S = 1000.0f;
	if (PicoLastActivity)
		S = (PicoGetDate() - PicoLastActivity) * 0.000015258789f; // seconds
	int x = (int)std::clamp(S * S * 50000.0f, 1000000.0f, 999999999.0f);
	timespec ts = {0, x};
	nanosleep(&ts, 0);
}


static void* pico_worker (void* T) {
	while (true) pico_work_comms();
}

bool pico_start () {
	if (PicoList.Count >= PicoMaxConnections)
		return puts("Pico: Too many threads.")*false;
	if (PicoThreads[0]) return true;
	for (int i = 0; i < 1; i++) {
		auto T = &PicoThreads[i];
		if (pthread_create(T, 0, pico_worker, 0) or pthread_detach(*T))
			return puts("Pico: Unable to create threads.")*false;
	}
	return true;
}

#endif



/// C-API ///
extern "C" PicoComms* PicoMsgComms (int Noise=PicoNoisy)  _pico_code_ (
	if (!pico_start()) return nullptr;
	return new PicoComms(Noise, true);
)

extern "C" PicoComms* PicoMsgCommsPair (PicoComms** Child, int Noise=PicoNoisy) _pico_code_ (
	PicoComms* M = PicoMsgComms(Noise);
	if (!M) return nullptr;
	*Child = M->Pair();
	return M;
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

extern "C" bool PicoMsgSend (PicoComms* M, PicoMessage Msg) _pico_code_ (
	if  (!Msg.Data  or  Msg.Length <= 0) return false;
	return M->QueueSend(Msg.Data, Msg.Length);
)

extern "C" bool PicoMsgSendStr (PicoComms* M, const char* Msg) _pico_code_ (
	return M->QueueSend(Msg, (int)strlen(Msg)+1);
)

extern "C" PicoMessage PicoMsgGet (PicoComms* M, float Time=0) _pico_code_ (
	return M->Get(Time);
)

extern "C" PicoMessageConfig* PicoMsgConf (PicoComms* M) _pico_code_ (
	return &M->Conf;
)

#endif
