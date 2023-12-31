

// some simple tests :)
#define PICO_IMPLEMENTATION
#include "PicoMsg.h"
#include <unistd.h>
#include <vector>
using std::vector; 



void* Query (PicoComms* M) {
	PicoMsgSay(M,"QA");
	PicoMsgSend(M, "mary had a little lamb");
	const int Stack = 10; const int Pudge = 4096;
	vector<char> abcd(Stack*Pudge);
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] = rand();
	}
	
	vector<PicoMessage> Sent;
	char* abc = &abcd[0];
	int Remain = Stack*Pudge;
	while (Remain > 0) {
		if (PicoMsgErr(M)) return 0;

		int n = rand() % Pudge;
		if (n > Remain) n = Remain;
		Remain -= n;
		Sent.push_back({abc, n});
		PicoMsgSend(M, abc, n);
		abc += n;
	}
	
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] ++;
	}

	PicoMsgSay(M, "QC");
	sleep(2); // give it a chance to receive
	for (auto v: Sent) {
		auto OK = PicoMsgGet(M, 1.0);
		if (!OK)								return 0;
		if (OK.Length != v.Length)				return PicoMsgSay(M, "bad length");
		int diff = memcmp(v.Data, OK.Data, v.Length);
		if (diff)								return PicoMsgSay(M, "netdown");
		abc += v.Length;
		PicoMsgSay(M, "Passed");
		free(OK.Data);
	}
	PicoMsgSay(M, "QD");
	return 0;
}


void Respond (PicoComms* M) {
	sleep(1); // give it a chance to receive
	auto Mary = PicoMsgGet(M);
	if (Mary) {
		puts(Mary.Data);
		free(Mary.Data);
	}
	
	while (!PicoMsgErr(M)) {
		auto OK = PicoMsgGet(M, 10.0);
		if (!OK) break;

		for (int i = 0; i < OK.Length; i++) {
			(OK.Data)[i]++;
		}
		PicoMsgSend(M, OK.Data, OK.Length);
		free(OK.Data); // or store it in the msg?
	}
}


int main (int argc, const char * argv[]) {
	int PID = 0;
	{
		auto M = PicoMsg(PicoNoisy);
		PID = PicoMsgFork(M);
		if (PID < 0)
			return PicoMsgErr(M);
		if (!PID) {
			PicoMsgConfig(M)->Name = "Respond";
			Respond(M);
		} else {
			PicoMsgConfig(M)->Name = "Query";
			Query(M);
		}
		PicoMsgDestroy(M);
	}	
		
	return 0;
}


//					dup2(FD, SavedParentIPC); // store it... so restarted processes can pick it up again.
