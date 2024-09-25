Release checklist
- [ ] Check outstanding issues on JIRA and Github.
- [ ] Check [latest documentation](https://python-zlib-ng.readthedocs.io/en/latest/) looks fine.
- [ ] Create a release branch.
  - [ ] Change current development version in `CHANGELOG.rst` to stable version.
- [ ] Check if the address sanitizer does not find any problems using `tox -e asan`
- [ ] Merge the release branch into `main`.
- [ ] Created an annotated tag with the stable version number. Include changes 
from CHANGELOG.rst.
- [ ] Push tag to remote. This triggers the wheel/sdist build on github CI.
- [ ] merge `main` branch back into `develop`.
- [ ] Build the new tag on readthedocs. Only build the last patch version of
each minor version. So `1.1.1` and `1.2.0` but not `1.1.0`, `1.1.1` and `1.2.0`.
- [ ] Create a new release on github.
- [ ] Update the package on conda-forge.
