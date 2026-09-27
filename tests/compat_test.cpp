#include <cstdio>
#include "../src/core/project.h"
using namespace vs;
int main(){
  int fails=0;
  // SHA-256 known answers (FIPS 180-2 plus padding edge cases 55/56/64 bytes)
  struct KA { string m; const char* h; };
  KA ka[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
    {string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
    {string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
  };
  for (auto& k : ka) if (sha256Hex(k.m) != k.h) { printf("sha mismatch len=%zu\n", k.m.size()); fails++; }
  Project P; P.addSample(false); P.spec.type=AnaType::Cooc; P.spec.unit=Unit::Keywords; P.spec.setDefaults(); P.build(nullptr,nullptr);
  // old 1.4 format: full tree, pretty
  string old = P.toJson(true).dump(1);
  writeFile("/tmp/old.vosproj", old);
  Project Q; string err; bool o=Q.open("/tmp/old.vosproj",&err);
  printf("old open=%d err='%s' recs=%zu/%zu n=%zu\n",o,err.c_str(),Q.corpus.recs.size(),P.corpus.recs.size(),size_t(Q.net.n()));
  if(!o||!err.empty()||Q.corpus.recs.size()!=P.corpus.recs.size()) fails++;
  // non-ASCII path, new format
  string np="/tmp/প্রকল্প é.vosproj"; string e2;
  if(!P.save(np,&e2)) { printf("save fail %s\n",e2.c_str()); fails++; }
  Project R; bool o2=R.open(np,&e2); printf("new open=%d err='%s' recs=%zu sha=%d\n",o2,e2.c_str(),R.corpus.recs.size(), R.toJson(false)["corpus"]["sha256"].str()==P.toJson(false)["corpus"]["sha256"].str());
  if(!o2||!e2.empty()) fails++;
  printf("fails=%d\n",fails);
}
