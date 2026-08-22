#include <par2/libpar2.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>

struct Trace : Par2::Par2Observer {
  bool verbose;
  explicit Trace(bool v = true) : verbose(v) {}
  int onfile = 0, onfiledone = 0, onsetinfo = 0, onrepair = 0, onprogress = 0;
  Par2::u32 lastpermille = 0; bool progress_went_back = false;
  void OnSetInfo(const Par2::Par2SetInfo &) override { onsetinfo++; }
  void OnFile(const std::string &n) override {
    onfile++; if (verbose) std::cout << "     OnFile(" << n << ")\n";
  }
  void OnFileDone(const std::string &n, Par2::u32 f, Par2::u32 need) override {
    onfiledone++; if (verbose) std::cout << "     OnFileDone(" << n << ", " << f << "/" << need << ")\n";
  }
  std::vector<Par2::u32> permilles;
  void OnProgress(Par2::u32 p) override {
    onprogress++; permilles.push_back(p);
    if (p < lastpermille) progress_went_back = true; lastpermille = p;
  }
  void OnRepairStart(void) override { onrepair++; if (verbose) std::cout << "     OnRepairStart\n"; }
};

static void dump(Par2::Par2Verifier &v, const char *label) {
  Par2::Par2VerifyResult r;
  if (!v.GetVerifyResult(&r)) { std::cout << "     " << label << ": GetVerifyResult=false\n"; return; }
  std::cout << "     " << label << ": complete=" << r.completefilecount
            << " renamed=" << r.renamedfilecount << " damaged=" << r.damagedfilecount
            << " missing=" << r.missingfilecount << " avail=" << r.availableblockcount
            << " missingblocks=" << r.missingblockcount
            << " recovery=" << r.recoveryblockcount << "\n";
}

int main(int argc, char **argv) {
  std::string scenario = argv[1], par2 = argv[2], base = argv[3];
  std::ostringstream out, err;
  Par2::Par2Verifier v(out, err, Par2::nlSilent, base);
  Trace t;
  v.SetObserver(&t);

  if (scenario == "getinfo-before-add") {
    Par2::Par2SetInfo i; std::vector<Par2::Par2FileInfo> f;
    std::cout << "     GetSetInfo before AddPar2File = " << v.GetSetInfo(&i) << "\n";
    std::cout << "     GetFileInfo before AddPar2File = " << v.GetFileInfo(&f) << "\n";
    Par2::Par2VerifyResult r;
    std::cout << "     GetVerifyResult before Verify = " << v.GetVerifyResult(&r) << "\n";
    std::cout << "     Reassess before Verify = " << (int)v.Reassess() << "\n";
    return 0;
  }
  if (scenario == "progress") {
    v.AddPar2File(par2);
    std::cout << "     --- permille during Verify ---\n     ";
    v.Verify({}, true, 0);
    for (auto p : t.permilles) std::cout << p << " ";
    std::cout << "\n     --- permille during Repair ---\n     ";
    t.permilles.clear();
    v.Repair();
    for (auto p : t.permilles) std::cout << p << " ";
    std::cout << "\n";
    return 0;
  }
  if (scenario == "missing-par2") {
    std::cout << "     AddPar2File(nonexistent) = " << (int)v.AddPar2File(par2 + ".nope") << "\n";
    return 0;
  }

  std::cout << "     AddPar2File = " << (int)v.AddPar2File(par2) << "\n";
  std::cout << "     OnSetInfo calls = " << t.onsetinfo << "\n";

  if (scenario == "verify" || scenario == "verify-twice" || scenario == "repair" || scenario == "rename") {
    Par2::Result r = v.Verify({}, true, 0);
    std::cout << "     Verify = " << (int)r << "\n";
    dump(v, "after verify");
    std::cout << "     observer: OnFile=" << t.onfile << " OnFileDone=" << t.onfiledone
              << " OnProgress=" << t.onprogress << " progress_went_backwards="
              << t.progress_went_back << "\n";
  }
  if (scenario == "verify-twice") {
    std::cout << "     --- second Verify on the same object ---\n";
    t.onfile = t.onfiledone = 0;
    Par2::Result r = v.Verify({}, true, 0);
    std::cout << "     Verify = " << (int)r << "\n";
    std::cout << "     observer: OnFile=" << t.onfile << " OnFileDone=" << t.onfiledone << "\n";
  }
  if (scenario == "repair" || scenario == "rename") {
    std::cout << "     Repair = " << (int)v.Repair() << "\n";
    dump(v, "after repair");
    std::vector<std::string> b;
    std::cout << "     GetBackupFiles = " << v.GetBackupFiles(&b) << " (" << b.size() << ")\n";
    for (auto &s : b) std::cout << "       backup: " << s << "\n";
    std::vector<Par2::Par2FileInfo> f; v.GetFileInfo(&f);
    for (auto &i : f) std::cout << "       fileinfo: " << i.filename << "\n";
  }
  return 0;
}
