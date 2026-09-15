# engine/ui/brand

The rx wordmark, as vector paths. `rx_engine.svg` is embedded in every rx
binary (`rx_embed_bytes` in `engine/ui/CMakeLists.txt`) and rasterized at
startup by `ui::Splash`, so the splash plate cannot be missing a logo on a
machine that never unpacked an archive.

One deviation from the supplied artwork: the opaque `<rect id="background">`
covering the canvas is gone. An engine asset has to composite onto whatever
backdrop the splash paints, and the rect also defeats the transparent-margin
trim that sizes the logo (`Splash::LoadLogo`). Every glyph and gradient is
otherwise untouched.

Charcoal `#363a3b`, red `#a91b1e`; `ui::kBrandRed` mirrors the latter for the
splash rule.
