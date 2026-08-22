#include <par2/libpar2.h>
#include <iostream>
#include <sstream>
int main(int argc, char **argv) {
  std::ostringstream out, err;
  std::string base = argv[2];
  Par2::Par2Verifier v(out, err, Par2::nlSilent, base);
  std::cerr << "step: AddPar2File" << std::endl;
  std::cerr << "  = " << (int)v.AddPar2File(argv[1]) << std::endl;
  std::cerr << "step: Verify" << std::endl;
  Par2::Result r = v.Verify({}, true, 0);
  std::cerr << "  = " << (int)r << std::endl;
  Par2::Par2VerifyResult vr;
  if (v.GetVerifyResult(&vr))
    std::cerr << "  complete=" << vr.completefilecount << " renamed=" << vr.renamedfilecount
              << " damaged=" << vr.damagedfilecount << " missing=" << vr.missingfilecount
              << " missingblocks=" << vr.missingblockcount << std::endl;
  std::cerr << "step: Repair" << std::endl;
  std::cerr << "  = " << (int)v.Repair() << std::endl;
  std::cerr << "step: GetBackupFiles" << std::endl;
  std::vector<std::string> b;
  std::cerr << "  = " << v.GetBackupFiles(&b) << " count=" << b.size() << std::endl;
  std::cerr << "step: done" << std::endl;
  return 0;
}
