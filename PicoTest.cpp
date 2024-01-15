
#define PICO_IMPLEMENTATION
#include "PicoMsg.h"
#include <unistd.h>
#include <vector>
using std::vector;


uint hash (uint x) {
	x ^= x >> 16;
	x *= UINT32_C(0x7feb352d);
	x ^= x >> 15;
	x *= UINT32_C(0x846ca68b);
	x ^= x >> 16;
	return x;
}


void* ThreadQuery (void* TM) {
	PicoComms* M = (PicoComms*)TM;
	M->Conf.Name = "Query";
	PicoSendStr(M, "mary had a little lamb");
	const int Stack = 49; const int Pudge = 4096;
	vector<char> abcd(Stack*Pudge);
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] = (char)(hash(i)%26)+'A';
	}
	
	vector<PicoMessage> Sent;
	vector<PicoMessage> Got;
	char* abc = &abcd[0];
	int Remain = Stack*Pudge;
	while (Remain > 0) {
		if (PicoError(M)) return PicoSay(M, "Exit: CantSendAll");

		int n = hash(Remain) % Pudge;
		if (n > Remain) n = Remain;
		if (n < 1) n = 1;
		Remain -= n;
		Sent.push_back({n, abc});
		PicoSend(M, {n, abc}, PicoSendCanTimeOut);
		auto Back = PicoGet(M);
		if (Back) {
			PicoSay(M, "User Got:", "", Back.Length);
			Got.push_back(Back);
		}
		abc += n;
	}
	
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] ++;
	}

	PicoSay(M, "Comparing", "", (int)Sent.size());
	
	while (auto Back = PicoGet(M, 7*(Sent.size() > Got.size()))) {
		Got.push_back(Back);
	}

	if (int Diff = (int)(Sent.size() - Got.size()); Diff) 
		return PicoSay(M, "Missing Responses: ", "", Diff);

	for (int i = 0; i < Sent.size(); i++) {
		auto& v = Sent[i];
		auto& OK = Got[i];
		if (OK.Length != v.Length)				return PicoSay(M, "Exit: BadLength");
		int diff = memcmp(v.Data, OK.Data, v.Length);
		if (diff)								return PicoSay(M, "Exit: BadContents");
		abc += v.Length;
		PicoSay(M, "Passed", "", i+1);
		free(OK.Data);
	}
	PicoClose(M);
	return PicoSay(M, "Exit: Tests Passed!");
}


bool GetAndSay (PicoComms* M, float t = 0, bool Final=false) {
	auto Mary = PicoGet(M, t);
	if (Mary.Data) {
		PicoSay(M, "Got", Mary.Data);
		free(Mary.Data);
		return true;
	}
	if (t > 0 and !Final)
		PicoSay(M, "failed get");
	return false;
}


bool ThreadRespond (PicoComms* M) {
	M->Conf.Name = "ThreadRespond";
	GetAndSay(M, 6.0);
	
	int n = 0;
	while (!PicoError(M)) {
		auto Msg = PicoGet(M, 1.3);
		if (!Msg) {
			PicoSay(M, "NoMoreInput", "", n);
			break;
		}

		for (int i = 0; i < Msg.Length; i++) {
			(Msg.Data)[i]++;
		}
		PicoSend(M, Msg, PicoSendCanTimeOut);
		free(Msg.Data);
		n++;
	}
	
	PicoSay(M, "Responses Given:", "", n);
	return true;
}


int TestPair (PicoComms* C) {
	auto C2 = PicoStartChild(C);
	if (C2) {
		PicoSendStr(C, "pear🍐🍐🍐test");
		GetAndSay(C2, 2.0);
	}
	PicoDestroy(C2);
	return 0;	
}


int TestWrite(char* Out, int i, char Base=0) {
	int j = 0;
	for (int reps = 0; reps <= i % 3; reps++) {
		int x = i;
		while (j < 16) {
			int y = x % 26;
			x /= 26;
			Out[j++] = y + 'A' + Base;
			if (x == 0)
				break;
		}
	}
	Out[j] = 0;
	return j+1;
}


int RecIndex = 0;
bool TestIntenseCompare (PicoComms* C, float T) {
	char Expected[20] = {};
	auto Rec = PicoGet(C, T); if (!Rec) return false;
	auto Found = Rec.Data;
	TestWrite(Expected, RecIndex);
	printf("<-- %s %i\n", Found, RecIndex);
	bool Rz = strcmp(Expected, Found) == 0;
	if (!Rz) {
		printf("String %i differed! Expected '%s' but found '%s'\n", RecIndex, Expected, Found);
		T = T;
	}
	RecIndex++;
	free(Found);
	return Rz;
}


int TestFork (PicoComms* C) {
	int PID = PicoStartFork(C);
	C->Conf.Noise = PicoNoiseAll;
	if (PID < 0)
		return -PID;
	if (PID) {
		C->Conf.Name = "Tester";
		char Out[20] = {}; memset(Out, -1, sizeof(Out));
		PicoMessage Snd = {0, Out};
		PicoSay(C, "Asks intensely");
		int MaxTests = 100000;
		for (int i = 0; i < MaxTests; i++) {
			Snd.Length = TestWrite(Out, i, 1);
			if (!PicoSend(C, Snd))
				return !PicoSay(C, "Exitting Sadly");
			PicoSay(C, "Sending", Snd.Data, i);
			while (TestIntenseCompare(C, 0)) {;}
		}
		PicoSay(C, "AllSent!");
		while (RecIndex < MaxTests and TestIntenseCompare(C, 10)) {;}
		PicoSay(C, "strings compared", "", RecIndex);
		PicoClose(C);

	} else {
		C->Conf.Name = "Fixer";
		int Back = 0;
		char Out[20] = {}; memset(Out,-1,sizeof(Out));
		while (auto Msg = PicoGet(C, 10.0)) {
			auto D = Msg.Data; Msg.Data = Out;
			int n = Msg.Length-1;
			for (int j = 0; j < n; j++)
				Out[j] = D[j] - 1;
			Out[n] = 0;

			if (!PicoSend(C, Msg))
				return !PicoSay(C, "Exitting Sadly");
			free(D);
			Back++;
			if (PicoError(C)) break;
		}
		PicoSay(C, "NoMoreInput", "", Back);
		
		while (PicoStillSending(C))
			sleep(1);
	}
	
	PicoSay(C, "Acheived"); return 0;	
}

int TestThread (PicoComms* C) {
	if (!PicoStartThread(C, (PicoThreadFn)ThreadRespond)) return -1;
	ThreadQuery(C);
	sleep(2); // let ThreadRespond exit
	return 0;	
}


extern char **environ;

int TestExec (PicoComms* C, const char* self) {
	// so... fork, exec, then send messages to it.
	int PID = PicoStartFork(C, true);
	if (!PID) { // we are the child... lets exec ourself
		const char* args[3] = {self, "exec", 0};
		return execve(self, (char**)args, environ); // in case it failed.
	}

	char Data[20];
	int Back = 0;
	PicoMessage M = {0, Data};
	for (int i = 0; i < 10000; i++) {
		M.Length = TestWrite(Data, i+999);
		if (PicoSend(C, M))
			PicoSay(C, "Sent", Data);
		Back += GetAndSay(C);
	}
	sleep(1);
	while (GetAndSay(C, 1, true)) {Back++;}
	
	PicoSay(C, "Total", "", Back);
	return 0;
}


int TestExec2 (PicoComms* C) {
	if (!PicoCompleteExec(C)) return -1;
	while (auto M = PicoGet(C, 1)) {
		if (!M)
			return 0;
		PicoSay(C, "Got", M.Data);
		for (int i = 0; i < M.Length-1; i++)
			M.Data[i]++;
		PicoSend(C, M);
	}
	return 0;
}


int main (int argc, const char * argv[]) {
	int rz = 0;
	const char* S = argv[1];
	if (!S) S = "1";
	auto C = PicoCreate();
	if (strcmp(S, "1")==0)
		rz = TestFork(C);
	  else if (strcmp(S, "2")==0)
		rz = TestPair(C);
	  else if (strcmp(S, "3")==0)
		rz = TestExec(C, argv[0]);
	  else if (strcmp(S, "exec")==0)
		return TestExec2(C);
	  else
		rz = TestThread(C);
	PicoDestroy(C);
	return rz;
}

