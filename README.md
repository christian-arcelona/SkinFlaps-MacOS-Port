# SkinFlaps for macOS

A macOS (Apple silicon) port of [SkinFlaps](https://github.com/uwgraphics/SkinFlaps),
the soft-tissue surgical simulator by Qisi Wang, Court Cutting MD and
Eftychios Sifakis. Built against upstream v1.2.1
(commit [`f88f5dc`](https://github.com/uwgraphics/SkinFlaps/commit/f88f5dc88d593db467a69005ee1f8d977a8df8a4)).

The solver is unchanged — the same physics constants, tolerances, timestep
and local–global iteration structure as the Windows build — and the example
procedures replay to the same results. Only the platform backends differ.
When an action cannot be completed, the port reports why and the session
continues; see [PORTING.md](PORTING.md) for those cases and for the
backend mapping.

## Download

[**Latest release**](https://github.com/christian-arcelona/SkinFlaps-MacOS-Port/releases/latest)
— download the `.pkg` and double-click it. Nothing else needs to be
installed.

Requires macOS 15 or later on an Apple silicon Mac. The installer presents
the license and the surgical disclaimer, places SkinFlaps in `/Applications`,
and installs over any earlier version. The anatomical models ship read-only
inside the app; on first launch it creates `SkinFlaps/History` in your home
folder, preloaded with the example procedures, and your own saved procedures
go there too.

**Educational use only.** Do not use this simulator to plan surgery for a
real patient. It has not been calibrated with data from cadavers or live
patients and makes no guarantee of surgical validity.

## What SkinFlaps is

*Adapted from the [upstream README](https://github.com/uwgraphics/SkinFlaps).*

This open source project provides a soft tissue surgical simulation program
which allows a surgeon to experiment with his/her own surgical designs to
solve a soft tissue surgery problem. The technical details regarding the
inner workings of the code as well as the motivations for its use may be
found in this *[paper][1]*.

The physics library is a highly efficient implementation of
*[projective dynamics][2]*, with limited collision handling. It was written
by Qisi Wang under the direction of her dissertation advisor Eftychios
Sifakis at the Computer Graphics Laboratory, Department of Computer Science,
University of Wisconsin (Madison), and is described in detail in a
*[paper by Wang Q, et al][3]*. Multiresolution tetrahedral physics
(*[see paper by Wang Q, et al][4]*) reduces the initial tetrahedron count for
the facial flaps model from 620,000 to 17,000. As the surgeon operates on
part of the model, only that subvolume is promoted to high resolution,
allowing fine incision detail to be rendered with high accuracy while the
less important peripheral parts stay coarse.

The surgical interface, models and graphics for the project were written by
Court Cutting MD of the Department of Plastic Surgery, NYU - Grossman School
of Medicine as an extension of previous *[surgical animation][5]* work in
conjunction with the Smile Train charity and a more recent attempt at a
*[finite element implementation][6]* of skin flap surgery.

On macOS the math and threading backends are Apple Accelerate (dense
BLAS/LAPACK), [SuiteSparse](https://people.engr.tamu.edu/davis/suitesparse.html)
CHOLMOD (sparse Cholesky), [oneTBB](https://github.com/uxlfoundation/oneTBB)
(tasking) and [SIMDe](https://github.com/simd-everywhere/simde) (AVX to NEON
translation).

## Mac controls

Everything in the video tutorials works the same way.

| | |
|---|---|
| Rotate | left drag |
| Zoom | scroll wheel, or two-finger scroll |
| Pan | Shift or Control + left drag (middle drag on a three-button mouse, as on Windows) |
| Surgical tools | right button — two-finger click on a trackpad, secondary click on a Magic Mouse |
| Tool modifier | Shift or Control, as on Windows |
| Delete selected hook or suture | Delete |
| Undo / redo | Cmd+Z / Cmd+Shift+Z |

The modifier held at the first or last knife point makes a T-in or T-out;
held on the second suture it lays an automatic row of sutures; held on a
deep-cut post it leaves that end of the cut open; held on an undermine point
it places a point that does not connect to the nearest incision.

Undo and redo cover every forward move — a fence post, an undermine point, a
hook, a suture, a cut — so a post placed in the wrong spot is taken back with
Cmd+Z. The Edit menu lists the moves that can be undone or redone.

If a user's action fails, a dialogue box will explain the error. Closing the
dialogue box will return the simulation to the state preceding the failed
action. Session history and log files can be saved with File > Save session
files...; they are kept in the `~/SkinFlaps/History` folder, which File >
Show session folder opens.

## Examples

Press **NEXT** after the program loads, then choose a file from your
`SkinFlaps/History` folder.

Facial flap procedures:

 - `AbbeEstlanderLip.hst` — reconstruction of upper lip defect
 - `AntiaBuch_ear.hst` — helical rim advancement closure of an ear defect
 - `cervicoFacialFlap.hst` — rotation flap closure of upper cheek defect
 - `cheekSplasty.hst` — cheek defect closure
 - `foreheadFlapToNose.hst` — paramedian forehead flap closure of nasal defect
 - `postAuricularEar.hst` — ear defect closed with postauricular flap and skin graft
 - `scalpDoubleRotation.hst` — scalp defect closed with two flaps
 - `TenzelEyelid.hst` — eyelid defect closed with lateral canthotomy and a semicircular flap

Cleft lip repairs:

 - `cleft_CuttingRepair.hst` — Dr. Cutting's repair of a complete unilateral cleft lip
 - `cleft_FisherRepair.hst` — David Fisher's repair of cleft lip
 - `cleft_RoseThompson.hst` — Rose/Thompson repair of cleft lip
 - `cleft_TennisonRandall.hst` — Tennison/Randall repair of cleft lip

In *[Dr. Cutting's YouTube channel][7]* there are two video tutorials on the
use of the software. The first demonstrates basic use of the program to
simulate facial flap closure of skin defects in *[Users Guide][8]*. A second,
more advanced tutorial on the use of SkinFlaps to simulate cleft lip/nose
repair can be found in *[Cleft lip tutorial][9]*.

## Reporting a problem

Report problems by opening an issue on GitHub at
<https://github.com/christian-arcelona/SkinFlaps-MacOS-Port/issues>. Please
describe what you were doing when the failure occurred and include the
session files: after the failure, choose File > Save session files... and
attach the `.hst`, `_full.hst` and `.log` files it writes as a `.zip`
(GitHub does not accept `.hst` files directly).

## Building from source (optional)

- macOS 15 or later, Apple silicon
- Xcode command line tools: `xcode-select --install`
- `brew install cmake pkg-config suite-sparse tbb glfw`

```
git clone --recurse-submodules https://github.com/christian-arcelona/SkinFlaps-MacOS-Port.git
cd SkinFlaps-MacOS-Port
cmake --preset gui
cmake --build build/gui -j --target SkinFlaps
open build/gui/SkinFlaps/SkinFlaps.app
```

The submodules are required: Eigen and SIMDe are compiled into the solver,
and the upstream tree is what the byte-identity check and the installer
compare against.

`gui-perf` is the optimized preset (`-O3`, thin LTO, Apple silicon tuning).
Both presets keep floating-point contraction and fast-math off, because those
flags change simulation results.

To build the installer package, add `brew install dylibbundler` and run
`installer/build_installer.sh <version>`. Signing is ad-hoc unless Developer
ID identities are supplied; see the top of that script.

## How the port is organized

`SkinFlaps/src` and the `gl3wGraphics` sources are byte-identical to the
upstream Windows tree pinned in `vendor/original_windows`. Every
macOS-specific change lives in `overlays/`, substituted onto the build
targets at configure time, so the upstream tree stays reviewable and its
updates merge cleanly. [PORTING.md](PORTING.md) covers the backend mapping,
the input layer, the failure handling and the solver architecture.

## Known Issues for Future Work

 1. All flap stretch limits are currently set to the same parameter. This is
    certainly untrue as it is known that flaps in different parts of the face
    have different stretch characteristics (e.g. cheek and eyelid skin
    stretches much more than scalp and forehead).
 2. Collision response may be inadequate in areas where a tight flap closure
    is done over a very convex surface. Increased collision density is
    planned in future iterations of the code.

## Code Owners

##### Surgical physics library — Qisi Wang and Eftychios Sifakis

##### Surgical interface, tools and graphics — Court Cutting MD (@ccuttingmd)

##### macOS port — Christian Arcelona (@christian-arcelona)

The authors wish to thank the team at AdvancedInstaller Inc. for the use of
their excellent Windows software installer.

## License

<a href="http://opensource.org/licenses/BSD-3-Clause">
<img align="right" src="http://opensource.org/trademarks/opensource/OSI-Approved-License-100x137.png">
</a>

    Copyright 2014-2022 Qisi Wang, Court Cutting, Eftychios Sifakis

    Redistribution and use in source and binary forms, with or without modification,
    are permitted provided that the following conditions are met (BSD-3-Clause license
    with extended surgical disclaimer; see the LICENSE file):

       1. Redistributions of source code must retain the above copyright notice, this
          list of conditions and the following disclaimer.

       2. Redistributions in binary form must reproduce the above copyright notice,
          this list of conditions and the following disclaimer in the documentation
          and/or other materials provided with the distribution.

       3. Neither the name of the copyright holder nor the names of its contributors
          may be used to endorse or promote products derived from this software without
          specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
    THE AUTHORS MAKE NO CLAIM OF SURGICAL ACCURACY FOR ANY PARTICULAR PATIENT. A
    PROGRAM GENERATED SURGICAL DESIGN THAT WORKS MAY NOT WORK IN A PATIENT. SIMILARLY
    A DESIGN THAT WORKS FOR A PARTICULAR PATIENT MAY NOT CLOSE THAT DEFECT IN THE PROGRAM.
    IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
    INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
    OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
    OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
    OF THE POSSIBILITY OF SUCH DAMAGE.


  [1]: https://doi.org/10.1016/j.cmpb.2022.106730
  [2]: https://www.cs.utah.edu/~ladislav/bouaziz14projective/bouaziz14projective.html
  [3]: https://onlinelibrary.wiley.com/doi/10.1111/cgf.14385
  [4]: https://journals.lww.com/prsgo/fulltext/10.1097/gox.0000000000006820~computer-based-simulation-of-facial-flap-and-cleft-lip
  [5]: https://www.tandfonline.com/doi/abs/10.3109/10929080209146521
  [6]: http://pages.cs.wisc.edu/~sifakis/papers/surgery_simulator_JRS.pdf
  [7]: https://www.youtube.com/@courtcutting2872
  [8]: https://www.youtube.com/watch?v=wXR3m3JQvo8&t=5s
  [9]: https://youtu.be/CzBiVJ5Q508
