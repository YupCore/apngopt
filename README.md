  APNG Optimizer 1.4.1

  Optimizes APNG animations.

  https://sourceforge.net/projects/apng/files/APNG_Optimizer/

  Copyright (c) 2011-2015 Max Stepin
  maxst@users.sourceforge.net

  License: zlib license

--------------------------------

  Usage:

`apngopt [options] anim.png [anim_opt.png]`

  Options:

```
-z0  : zlib compression (default, v1.4.1 修改，提高apng frame压缩速度)
-z1  : 7zip compression
-z2  : zopfli compression
-i## : number of iterations, default -i15
-d## : disable imagequant compress 0 or 1, default 0  (v1.4.1 添加，-d1 执行原1.4的逻辑)
```

  Build (MSVC only):

 
```
 %comspec% /k "D:\VS2026\VC\Auxiliary\Build\vcvars64.bat"

 # Release
 cmake -S . -B build-msvc-release -G Ninja -DCMAKE_BUILD_TYPE=Release
 cmake --build build-msvc-release --config Release
 .\build-msvc-release\apngopt.exe

 # Debug
 cmake -S . -B build-msvc-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
 cmake --build build-msvc-debug --config Debug
 .\build-msvc-debug\apngopt.exe
```

  Notes:

- Build uses vendored dependencies from `vendor/` (`zlib`, `libpng`, `zopfli`, `libimagequant`).
- Rust/Cargo is required to build `vendor/libimagequant/imagequant-sys`.
- `libimagequant` threading is Rayon-backed; control with `RAYON_NUM_THREADS`.
- `-z1` is wired to `vendor/7z2601` Deflate sources (LZMA SDK 26.01), while keeping the existing `compress_rfc1950_7z` call surface in `7z/7z.h`.


--------------------------------

 **Changes in version 1.4.1:** 

- 默认使用libimagequant进行压缩, 参照 https://pngquant.org/lib/
- 默认使用zlib压缩，提高压缩速度
- 添加-d参数，可以切换1.4的逻辑

Changes in version 1.4:

- Codebase updated (based on apngdis 2.8, apngasm 2.9)


Changes in version 1.3:

- Codebase updated (based on apngdis 2.7, apngasm 2.9)
- Added 7zip and Zopfli compression options.


  Changes in version 1.2:

- Codebase updated (based on apngdis 2.5, apngasm 2.7)
- Optimization: join identical frames


Changes in version 1.1:

- Codebase updated (based on apngdis 2.4, apngasm 2.5)
- Better optimization
- zlib license


Changes in version 1.0:

- Initial release (based on apngdis 2.3, apngasm 2.3)
