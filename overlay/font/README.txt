WenQuanYi Micro Hei (wqy-microhei.ttc)
=====================================

Bundled with this Simplified Chinese localization of OptiScaler.

It is used as the menu font because the font compiled into OptiScaler (Hack) only
carries Latin glyphs and cannot draw Chinese characters.

OptiScaler loads the first of these that exists:

  1. TTFFontPath from OptiScaler.ini, if set
  2. font\wqy-microhei.ttc next to OptiScaler.dll / the game executable
  3. wqy-microhei.ttc next to OptiScaler.dll / the game executable
  4. font\wqy-microhei.ttc relative to the working directory
  5. C:\Windows\Fonts\msyh.ttc        (Microsoft YaHei)
  6. C:\Windows\Fonts\msyh.ttf
  7. C:\Windows\Fonts\simhei.ttf      (SimHei)
  8. C:\Windows\Fonts\Deng.ttf        (DengXian)
  9. C:\Windows\Fonts\simsun.ttc      (SimSun)

If none is found the interface falls back to the Latin-only font and Chinese text
will not render.

Licence
-------
WenQuanYi Micro Hei is Copyright (C) 2008-2009 WenQuanYi Project.
It is released under the Apache License 2.0 and the GNU General Public License v3
with the font embedding exception. See:
  https://www.wenquanyi.org/
  http://wenq.org/wqy2/index.cgi?MicroHei

This file is only redistributed alongside the localization; OptiScaler itself is
licensed separately (see ../LICENSE).
