#!/usr/bin/env python3
"""Generates the frameworks/base patch that registers hwui's JNI entries
through System V thunks, as tools/jni-sysv-patch.py does for core/jni.

  compat/hwui/gen_sysv_patch.py <frameworks/base checkout> \
      > patches/frameworks/base/0101-graphics-hwui-sysv-jni-tables.patch

ART enters natives with the System V ABI (docs/decisions/0006-managed-abi.md);
hwui's JNI tables (libs/hwui/jni, and core/jni's Surface, built with hwui) are written by hand, `{"name", "sig",
(void*) fn}`. Each `(void*) fn` becomes `GESHER_JNI(fn)`, which
graphics_jni_helpers.h defines under GESHER (patch 0100). The sources are
read from the checkout's HEAD; FILES are the hwui JNI sources
cmake/aosp/hwui.cmake compiles that have tables.
"""
import difflib
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'jni_sysv_patch', os.path.join(HERE, '..', '..', 'tools', 'jni-sysv-patch.py'))
jsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jsp)

FILES = [
    'libs/hwui/jni/AnimatedImageDrawable.cpp',
    'libs/hwui/jni/Bitmap.cpp',
    'libs/hwui/jni/BitmapFactory.cpp',
    'libs/hwui/jni/BitmapRegionDecoder.cpp',
    'libs/hwui/jni/ByteBufferStreamAdaptor.cpp',
    'libs/hwui/jni/Camera.cpp',
    'libs/hwui/jni/CanvasProperty.cpp',
    'libs/hwui/jni/ColorFilter.cpp',
    'libs/hwui/jni/CreateJavaOutputStreamAdaptor.cpp',
    'libs/hwui/jni/FontFamily.cpp',
    'libs/hwui/jni/Gainmap.cpp',
    'libs/hwui/jni/Graphics.cpp',
    'libs/hwui/jni/ImageDecoder.cpp',
    'libs/hwui/jni/Interpolator.cpp',
    'libs/hwui/jni/MaskFilter.cpp',
    'libs/hwui/jni/MeshSpecification.cpp',
    'libs/hwui/jni/NinePatch.cpp',
    'libs/hwui/jni/Paint.cpp',
    'libs/hwui/jni/PaintFilter.cpp',
    'libs/hwui/jni/Path.cpp',
    'libs/hwui/jni/PathEffect.cpp',
    'libs/hwui/jni/PathIterator.cpp',
    'libs/hwui/jni/PathMeasure.cpp',
    'libs/hwui/jni/Picture.cpp',
    'libs/hwui/jni/Region.cpp',
    'libs/hwui/jni/RenderEffect.cpp',
    'libs/hwui/jni/RuntimeXfermode.cpp',
    'libs/hwui/jni/Shader.cpp',
    'libs/hwui/jni/Typeface.cpp',
    'libs/hwui/jni/android_graphics_Canvas.cpp',
    'libs/hwui/jni/android_graphics_Color.cpp',
    'libs/hwui/jni/android_graphics_ColorSpace.cpp',
    'libs/hwui/jni/android_graphics_DisplayListCanvas.cpp',
    'libs/hwui/jni/android_graphics_HardwareBufferRenderer.cpp',
    'libs/hwui/jni/android_graphics_HardwareRenderer.cpp',
    'libs/hwui/jni/android_graphics_HardwareRendererObserver.cpp',
    'libs/hwui/jni/android_graphics_Matrix.cpp',
    'libs/hwui/jni/android_graphics_Mesh.cpp',
    'libs/hwui/jni/android_graphics_Picture.cpp',
    'libs/hwui/jni/android_graphics_RenderNode.cpp',
    'libs/hwui/jni/android_graphics_animation_NativeInterpolatorFactory.cpp',
    'libs/hwui/jni/android_graphics_animation_RenderNodeAnimator.cpp',
    'libs/hwui/jni/android_graphics_drawable_AnimatedVectorDrawable.cpp',
    'libs/hwui/jni/android_graphics_drawable_VectorDrawable.cpp',
    'libs/hwui/jni/android_util_PathParser.cpp',
    'libs/hwui/jni/fonts/Font.cpp',
    'libs/hwui/jni/fonts/FontFamily.cpp',
    'libs/hwui/jni/text/GraphemeBreak.cpp',
    'libs/hwui/jni/text/LineBreaker.cpp',
    'libs/hwui/jni/text/MeasuredText.cpp',
    'libs/hwui/jni/text/TextShaper.cpp',
    # core/jni's Surface natives, in their host form, are built with hwui
    # (cmake/aosp/hwui.cmake); core_jni_helpers.h has GESHER_JNI (patch 0001).
    'core/jni/android_view_Surface.cpp',
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    checkout = sys.argv[1]
    sys.stdout.reconfigure(newline='\n')
    for path in FILES:
        old = subprocess.run(['git', '-C', checkout, 'show', 'HEAD:' + path], check=True,
                             capture_output=True).stdout.decode('utf-8')
        if 'JNINativeMethod' not in old:
            continue
        # jni-sysv-patch.py insists on core_jni_helpers.h; hwui's sources reach
        # GESHER_JNI through GraphicsJNI.h or graphics_jni_helpers.h instead.
        if not any(h in old for h in ('GraphicsJNI.h', 'graphics_jni_helpers.h',
                                      'core_jni_helpers.h')):
            sys.exit(f'{path}: includes none of GraphicsJNI.h, graphics_jni_helpers.h, '
                     'core_jni_helpers.h')
        marker = '\n// core_jni_helpers.h\n'
        new = jsp.rewrite(path, old + marker)
        assert new.endswith(marker)
        new = new[:-len(marker)]
        if new == old:
            continue
        sys.stdout.write(f'diff --git a/{path} b/{path}\n')
        sys.stdout.writelines(difflib.unified_diff(
            old.splitlines(keepends=True), new.splitlines(keepends=True),
            f'a/{path}', f'b/{path}', n=3))


if __name__ == '__main__':
    main()
