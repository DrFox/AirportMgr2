"""Downloads Inter and imports its two faces as UFontFace assets. Run in the FULL editor, which
this script then closes:

  UnrealEditor.exe <project> -ExecutePythonScript=<this file> -unattended -nosplash

NOT THE COMMANDLET. The font factory calls UFontFace::CacheSubFaces, which reaches Slate's font
cache; a -run=pythonscript commandlet has no FSlateApplication, and the import dies on
`Assertion failed: CurrentApplication.IsValid()` (SlateApplication.h:321, 2026-09-28).

FACES, NOT A UFont. A composite UFont cannot be authored from Python on 5.8: FFontData's
FontFaceAsset is a private UPROPERTY. UUIStyle builds the composite in C++ from these two faces
instead (FFontData's UObject constructor is public). An AUTOMATED import creates only the
UFontFace and skips the "create a Font asset too?" dialog (EditorFactories.cpp, UFontFileImportFactory).

EVERY DOWNLOAD IS VALIDATED, as fetch_ui_icons.py's are: a TTF starts 00 01 00 00, and anything
else is an HTML error page that would import as nothing.

OFL: Inter is SIL Open Font License 1.1; the licence text ships beside the faces.
"""
import os
import urllib.request
import zipfile

import unreal

URL = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
OUT_DIR = "/Game/UI/Fonts"
FACES = {
    "extras/ttf/Inter-Regular.ttf": "FF_Inter_Regular",
    "extras/ttf/Inter-SemiBold.ttf": "FF_Inter_SemiBold",
}
TTF_SIGNATURE = b"\x00\x01\x00\x00"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    cache = os.path.join(unreal.Paths.project_saved_dir(), "UIFonts")
    os.makedirs(cache, exist_ok=True)
    archive = os.path.join(cache, "Inter-4.1.zip")
    if not os.path.isfile(archive):
        urllib.request.urlretrieve(URL, archive)
    with zipfile.ZipFile(archive) as z:
        licence = z.read("LICENSE.txt")
        extracted = {}
        for member, asset_name in FACES.items():
            data = z.read(member)
            if data[:4] != TTF_SIGNATURE:
                fail("%s does not start with the TTF signature" % member)
                say("DONE")
                return
            local = os.path.join(cache, asset_name + ".ttf")
            with open(local, "wb") as f:
                f.write(data)
            extracted[asset_name] = local

    content_fonts = os.path.join(unreal.Paths.project_content_dir(), "UI", "Fonts")
    os.makedirs(content_fonts, exist_ok=True)
    with open(os.path.join(content_fonts, "OFL.txt"), "wb") as f:
        f.write(licence)

    tasks = []
    for asset_name, local in extracted.items():
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", local)
        task.set_editor_property("destination_path", OUT_DIR)
        task.set_editor_property("destination_name", asset_name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.FontFileImportFactory())
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    for asset_name in extracted:
        path = "%s/%s" % (OUT_DIR, asset_name)
        face = unreal.EditorAssetLibrary.load_asset(path)
        if face is None or not isinstance(face, unreal.FontFace):
            fail("%s did not import as a FontFace (got %r)" % (path, face))
            continue
        # INLINE: the face data is cooked into the asset, so a packaged build needs no loose .ttf.
        face.set_editor_property("loading_policy", unreal.FontLoadingPolicy.INLINE)
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        say("PASS %s is a FontFace" % path)
    say("DONE")


try:
    run()
finally:
    # The full editor stays open after -ExecutePythonScript; this script is a batch step.
    unreal.SystemLibrary.quit_editor()
