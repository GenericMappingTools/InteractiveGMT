---
name: gmtvtk-drape-resume
description: DONE — GMTimage draping over the gmtvtk grid surface; built + verified
metadata: 
  node_type: memory
  type: project
  originSessionId: cd809aef-dac3-4c16-b60b-bedb14b7135f
---

DONE (2026-06-12): **image draping** in the gmtvtk viewer. Context: [[gmtvtk-bridge]].
`view_grid(G; drape=I)` textures a `GMTimage` over the surface instead of CPT colour.
Built + verified: synthetic RGB ramp (R=east, G=north, B=64) draped over `GMT.peaks()`,
snapshot rendered correctly textured, scalar bar hidden.

WHAT SHIPPED (`qtvtk_proto/main.cpp` + `bridge.jl`, build `build.bat`):
- `makeGridFromArray`: bakes per-point tcoords (u=west→east, v=south→north) +
  `pd->GetPointData()->SetTCoords(tcoord)`.
- `buildAndShow(...)` gained `const unsigned char* img,int iw,int ih,int ibands` BEFORE
  `title`. When img set: builds `vtkImageData`(iw,ih,1; ibands UInt8; memcpy) + `vtkTexture`
  (InterpolateOn), `surf->SetTexture`, `map->ScalarVisibilityOff()`, `bar->VisibilityOff()`.
- `gmtvtk_view_grid(... ncolor, img,iw,ih,ibands, title)` — texture args after the CPT args.
- `gmtvtk_view_demo` buildAndShow call passes `nullptr,0,0,0` for the 4 texture args.
- `bridge.jl` `_drape_buf(I)`: **layout-aware** (mirrors GMTF3D `img_to_texbuf` in
  src/common.jl) — must read `I.layout`: char2=='R' → row-major array [lon,lat];
  char1!='B' → first lat index is north. Output buf row 0 = south (VTK origin), W→E;
  grey expands to RGB; returns (nlon,nlat,comps). Do NOT hard-assume (ny,nx,nb) — first
  cut ignored layout and row-major (TRBa) images came out left-right flipped (mat2img is
  TCBa so synthetic tests don't catch it; grdimage/gdal products are TRBa). `view_grid`
  gained `drape=nothing`; passes buf or `C_NULL,0,0,0`. ccall +`Ptr{Cuchar},Cint,Cint,Cint`.
  Verified both TCBa + forced-TRBa F-marker render identical (correct).

GOTCHAS confirmed: dlopen'd gmtvtk.dll locks the file → kill julia + test in FRESH session.
buildAndShow signature vs demo call arg-count must match. Verify only via `gmtvtk_save_png`
(GDI/PrintWindow do NOT capture the GL surface). Test script: `qtvtk_proto/test_drape.jl`.
