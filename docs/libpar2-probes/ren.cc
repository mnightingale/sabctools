#include <par2/libpar2.h>
#include <iostream>
#include <sstream>
int main(int argc, char **argv) {
  std::ostringstream out, err;
  std::string base = argv[2];
  Par2::Par2Verifier v(out, err, Par2::nlSilent, base);
  v.AddPar2File(argv[1]);
  std::vector<std::string> extra;
  for (int i = 3; i < argc; i++) extra.push_back(argv[i]);
  std::cout << "  Verify(extrafiles=" << extra.size() << ") = " << (int)v.Verify(extra, true, 0) << "\n";
  Par2::Par2VerifyResult r; v.GetVerifyResult(&r);
  std::cout << "  complete=" << r.completefilecount << " renamed=" << r.renamedfilecount
            << " damaged=" << r.damagedfilecount << " missing=" << r.missingfilecount << "\n";
  std::cout << "  Repair = " << (int)v.Repair() << "\n";
  std::vector<std::string> b; v.GetBackupFiles(&b);
  std::cout << "  GetBackupFiles count=" << b.size() << "\n";
  for (auto &s : b) std::cout << "    " << s << "\n";
  std::vector<Par2::Par2FileInfo> f; v.GetFileInfo(&f);
  std::cout << "  GetFileInfo reports set names and local paths:\n";
  for (auto &i : f) std::cout << "    " << i.filename << " -> " << i.localfilename << "\n";
  std::vector<std::pair<std::string, std::string> > ren;
  v.GetRenamedFiles(&ren);
  std::cout << "  GetRenamedFiles count=" << ren.size() << "\n";
  for (auto &i : ren) std::cout << "    " << i.first << " -> " << i.second << "\n";
  return 0;
}
