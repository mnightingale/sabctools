#include <par2/libpar2.h>
#include <iostream>
#include <sstream>
static void dump(Par2::Par2Verifier &v, const char *l) {
  Par2::Par2VerifyResult r;
  if (!v.GetVerifyResult(&r)) { std::cout << "  " << l << ": (none)\n"; return; }
  std::cout << "  " << l << ": complete=" << r.completefilecount << " damaged=" << r.damagedfilecount
            << " missing=" << r.missingfilecount << " avail=" << r.availableblockcount
            << " missingblocks=" << r.missingblockcount << " recovery=" << r.recoveryblockcount << "\n";
}
int main(int argc, char **argv) {
  std::string mode = argv[1], base = argv[4];
  std::ostringstream out, err;
  Par2::Par2Verifier v(out, err, Par2::nlSilent);

  if (mode == "reassess") {
    std::cout << "  AddPar2File(index only) = " << (int)v.AddPar2File(argv[2]) << "\n";
    std::cout << "  Verify = " << (int)v.Verify(base, {}, true, 0) << "\n";
    dump(v, "after verify");
    std::cout << "  AddPar2File(volume) = " << (int)v.AddPar2File(argv[3]) << "\n";
    std::cout << "  Reassess = " << (int)v.Reassess() << "\n";
    dump(v, "after reassess");
    std::cout << "  Repair = " << (int)v.Repair(base) << "\n";
    return 0;
  }
  if (mode == "knownblocks") {
    v.AddPar2File(argv[2]); v.AddPar2File(argv[3]);
    Par2::Par2SetInfo si; v.GetSetInfo(&si);
    std::vector<Par2::Par2FileInfo> files; v.GetFileInfo(&files);
    // Vouch for every block of every file: nothing should need reading.
    for (auto &f : files) {
      std::vector<char> all(f.blockcount, 1);
      v.SetKnownBlocks(f.filename, all);
      std::cout << "  vouched " << f.filename << " x" << f.blockcount << "\n";
    }
    std::cout << "  Verify = " << (int)v.Verify(base, {}, true, 0) << "\n";
    dump(v, "with known blocks");
    std::cout << "  -- now forget one file (empty vector) and re-verify --\n";
    v.SetKnownBlocks(files[0].filename, {});
    std::cout << "  Verify = " << (int)v.Verify(base, {}, true, 0) << "\n";
    dump(v, "after forgetting");
    return 0;
  }
  return 1;
}
