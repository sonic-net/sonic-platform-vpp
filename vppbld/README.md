## Introduction
vppbld Makefile tries to download the specified vpp debian packages from buildkite. If it is not found there, it then builds it locally by pulling source code and applying vpp.patch.

The version of vpp packages is specified in sonic-buildimage/platform/vpp/rules/vpp.mk. When we build target/sonic-vpp.img.gz or libvppinfra, for example target/debs/bookworm/libvppinfra_2506-0.2+b1sonic1_amd64.deb, it will trigger above Makefile.

If we need to create a new vpp debian packages, either we have a new patch or we are upgrading vpp base version, we can specify a new version in sonic-buildimage/platform/vpp/rules/vpp.mk then run one of the above make target.

## Updating the VPP Build

Use this flow when you need to refresh the packaged VPP build.

The release schedule is currently once a month. Changes merged in the previous month are included in the new release, and those changes do not take effect until that release.

1. Pick the upstream VPP reference.

	Check https://github.com/fdio/vpp/tags and choose the latest stable release tag. Do not use `-rc` tags. Update `vppbld/vpp_version` to the commit SHA for that stable release, or choose latest if newer changes are needed.

2. Update the package version in `rules/vpp.mk`.

	Set `VPP_VERSION_BASE` to the upstream major version in `YYMM` form. For example, upstream `v26.06` maps to `2606`.

	Set `VPP_VERSION` to `$(VPP_VERSION_BASE)-0.x`. If you are rebuilding for the same upstream major version, increment only the minor suffix (`0.x`). If you move to a new stable upstream major version, start a new minor sequence for that major version.

	`VPP_VERSION_SONIC` is derived as `$(VPP_VERSION)+b1sonic1`, so if `VPP_VERSION` is `2606-0.2`, the package version becomes `2606-0.2+b1sonic1`.

3. Build the packages.

	From the `sonic-buildimage` workspace that includes this repo as `platform/vpp`, run a full image build:

	```bash
	make target/sonic-vpp.img.gz
	```

	This triggers the VPP package build through `platform/vpp/vppbld`, using the reference from `vppbld/vpp_version` and the version from `platform/vpp/rules/vpp.mk`.

4. Collect the artifacts.

	The artifact names must match the `VPP_VERSION_SONIC` value from `rules/vpp.mk`. Collect these files from `target/debs/trixie/` in the `sonic-buildimage` workspace:

	- `libvppinfra_<VPP_VERSION_SONIC>_amd64.deb`
	- `libvppinfra-dev_<VPP_VERSION_SONIC>_amd64.deb`
	- `python3-vpp-api_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp-crypto-engines_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp-dbg_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp-dev_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp-plugin-core_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp-plugin-devtools_<VPP_VERSION_SONIC>_amd64.deb`
	- `vpp-plugin-dpdk_<VPP_VERSION_SONIC>_amd64.deb`

5. Upload the artifacts.

	Upload the collected Debian packages to the Buildkite package registry:

	https://buildkite.com/organizations/sonic-vpp/packages/registries/vpp

6. Raise a `sonic-platform-vpp` PR with the updated `vppbld/vpp_version` and `rules/vpp.mk`.

7. Create a draft PR in `sonic-buildimage` to trigger the `sonic-vpp` `sonic-mgmt` sanity test. Ensure the `platform/vpp` submodule is updated to point to the `sonic-platform-vpp` PR branch. Once passing, the sonic-platform-vpp PR can be merged.