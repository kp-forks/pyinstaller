# -*- coding: utf-8 -*-
#-----------------------------------------------------------------------------
# Copyright (c) 2005-2023, PyInstaller Development Team.
#
# Distributed under the terms of the GNU General Public License (version 2
# or later) with exception for distributing the bootloader.
#
# The full license is in the file COPYING.txt, distributed with this software.
#
# SPDX-License-Identifier: (GPL-2.0-or-later WITH Bootloader-exception)
#-----------------------------------------------------------------------------

import locale
import os
import sys
import pathlib
import subprocess
import re

import pytest

from PyInstaller.compat import is_darwin, is_win
from PyInstaller.utils.tests import importorskip, skipif, xfail


def test_run_from_path_environ(pyi_builder):
    pyi_builder.test_script('pyi_absolute_python_path.py', run_from_path=True)


@pytest.mark.linux
def test_absolute_ld_library_path(pyi_builder):
    pyi_builder.test_script('pyi_absolute_ld_library_path.py')


def test_absolute_python_path(pyi_builder):
    pyi_builder.test_script('pyi_absolute_python_path.py')


@pytest.mark.linux
@skipif(not os.path.exists('/proc/self/status'), reason='/proc/self/status does not exist')
@pytest.mark.parametrize("symlink_name", ["symlink", "very_long_name_in_symlink", "sub/dir/program"])
def test_symlink_basename_is_kept(pyi_builder_spec, symlink_name, tmp_path, spec_dir, script_dir):
    def _patch_spec(spec_name, symlink_name):
        spec_content = (spec_dir / spec_name).read_text(encoding="utf-8")
        spec_content = spec_content.replace("@SYMLINKNAME@", symlink_name)
        spec_content = spec_content.replace("@SCRIPTDIR@", str(script_dir))
        spec_file = tmp_path / spec_name
        spec_file.write_text(spec_content, encoding="utf-8")
        return spec_file

    spec_file = _patch_spec("symlink_basename_is_kept.spec", symlink_name)
    pyi_builder_spec.test_spec(str(spec_file), app_name=symlink_name)


def test_pyz_as_external_file(pyi_builder, monkeypatch):
    # This tests the not well documented and seldom used feature of having the PYZ-archive in a separate file (.pkg).

    def MyEXE(*args, **kwargs):
        kwargs['append_pkg'] = False
        return EXE(*args, **kwargs)

    # :todo: find a better way to not even run this test in onefile-mode
    if pyi_builder._mode == 'onefile':
        pytest.skip('only --onedir')

    import PyInstaller.building.build_main
    EXE = PyInstaller.building.build_main.EXE
    monkeypatch.setattr('PyInstaller.building.build_main.EXE', MyEXE)

    pyi_builder.test_source("print('Hello Python!')")


def test_base_modules_regex(pyi_builder):
    """
    Verify that the regex for excluding modules listed in PY3_BASE_MODULES does not exclude other modules.
    """
    pyi_builder.test_source("""
        import resources_testmod
        print('OK')
        """)


def test_celementtree(pyi_builder):
    pyi_builder.test_source("""
        from xml.etree.cElementTree import ElementTree
        print('OK')
        """)


# Test a build with some complexity with the ``noarchive`` debug option.
def test_noarchive(pyi_builder):
    pyi_builder.test_source("from xml.etree.cElementTree import ElementTree", pyi_args=['--debug=noarchive'])


@importorskip('codecs')
def test_codecs(pyi_builder):
    pyi_builder.test_script('pyi_codecs.py')


def test_compiled_filenames(pyi_builder):
    pyi_builder.test_source(
        """
        import pyi_dummy_module
        from os.path import isabs

        assert not isabs(pyi_dummy_module.dummy.__code__.co_filename), (
            "pyi_dummy_module.dummy.__code__.co_filename has compiled filename: %s" %
            (pyi_dummy_module.dummy.__code__.co_filename, )
        )
        assert not isabs(pyi_dummy_module.DummyClass.dummyMethod.__code__.co_filename), (
            "pyi_dummy_module.DummyClass.dummyMethod.__code__.co_filename has compiled filename: %s" %
            (pyi_dummy_module.DummyClass.dummyMethod.__code__.co_filename, )
        )
        """
    )


def test_decoders_ascii(pyi_builder):
    pyi_builder.test_source(
        """
        # Convert type 'bytes' to type 'str'.
        assert b'foo'.decode('ascii') == 'foo'
        """
    )


def test_distutils_submod(pyi_builder):
    # Test import of submodules of distutils package.
    # PyI fails to include `distutils.version` when running from virtualenv.
    pyi_builder.test_source("""
        from distutils.version import LooseVersion
        """)


def test_dynamic_module(pyi_builder):
    pyi_builder.test_source(
        """
        import pyi_testmod_dynamic

        # The value 'foo' should  not be None.
        print("'foo' value: %s" % pyi_testmod_dynamic.foo)
        assert pyi_testmod_dynamic.foo is not None
        assert pyi_testmod_dynamic.foo == 'A new value!'
        """
    )


def test_email(pyi_builder):
    pyi_builder.test_source(
        """
        from email import utils
        from email.header import Header
        from email.mime.text import MIMEText
        from email.mime.multipart import MIMEMultipart
        from email.mime.nonmultipart import MIMENonMultipart
        """
    )


def test_filename(pyi_builder):
    pyi_builder.test_script('pyi_filename.py')


def test_getfilesystemencoding(pyi_builder):
    pyi_builder.test_script('pyi_getfilesystemencoding.py')


def test_helloworld(pyi_builder):
    pyi_builder.test_source("print('Hello Python!')")


def test_module__file__attribute(pyi_builder):
    pyi_builder.test_script('pyi_module__file__attribute.py')


def test_module_attributes(tmp_path, pyi_builder):
    # Create a text file in tmp_path with path to the python executable and contents of PATH.
    # The frozen test program uses this information to spawn python interpreter to obtain attributes of the test modules
    # when running unfrozen, which it then compares to the attributes of the test modules within the frozen test
    # application itself.
    with open(tmp_path / 'python_exe.build', 'w', encoding='utf8') as f:
        f.write(sys.executable + "\n")
        f.write(os.environ.get('PATH') + '\n')
    pyi_builder.test_script('pyi_module_attributes.py')


def test_module_reload(pyi_builder):
    pyi_builder.test_script('pyi_module_reload.py')


def test_ctypes_hooks_are_in_place(pyi_builder):
    pyi_builder.test_source(
        """
        import ctypes
        assert ctypes.CDLL.__name__ == 'PyInstallerCDLL', ctypes.CDLL
        """
    )


def test_load_dll_using_ctypes(pyi_builder, compiled_dylib):
    # Collect the compiled shared library into top-level application directory
    pyi_builder.test_source(
        f"""
        import os
        import sys
        import ctypes

        libname = {str(compiled_dylib.name)!r}
        libpath = os.path.join(os.path.dirname(__file__), libname)

        lib = ctypes.CDLL(libpath)

        assert lib.add_twelve(42) == 42 + 12
        """,
        pyi_args=['--add-binary', f"{compiled_dylib}:."],
    )


def test_chdir_meipass(pyi_builder):
    # Ensure meipass dir exists.
    pyi_builder.test_source(
        """
        import os, sys
        os.chdir(sys._MEIPASS)
        print(os.getcwd())
        """
    )


def test_option_exclude_module(pyi_builder):
    """
    Test to ensure that when using option --exclude-module=xml.sax
    the module 'xml.sax' won't be bundled.
    """
    pyi_builder.test_source(
        """
        try:
            import xml.sax
            # Option --exclude-module=xml.sax did not work and the module
            # was successfully imported.
            raise SystemExit('Module xml.sax was excluded but it is '
                             'bundled with the executable.')
        except ImportError:
            # The Import error is expected since PyInstaller should
            # not bundle 'xml.sax' module.
            pass
        """,
        pyi_args=['--exclude-module', 'xml.sax']
    )


def test_option_verbose(pyi_builder, monkeypatch):
    """
    Test to ensure that option V can be set and has effect.
    """

    # This option is like 'python -v' - trace import statements.
    # 'None' should be allowed or '' also.

    def MyEXE(*args, **kwargs):
        args = list(args)
        args.append([('v', None, 'OPTION')])
        return EXE(*args, **kwargs)

    import PyInstaller.building.build_main
    EXE = PyInstaller.building.build_main.EXE
    monkeypatch.setattr('PyInstaller.building.build_main.EXE', MyEXE)

    pyi_builder.test_source(
        """
        print('test - PYTHONVERBOSE - trace import statements')
        import re # just import anything
        print('test - done')
        """
    )


def test_option_w_unset(pyi_builder):
    """
    Test to ensure that option W is not set by default.
    """
    pyi_builder.test_source("""
        import sys
        assert 'ignore' not in sys.warnoptions
        """)


def test_option_w_ignore(pyi_builder, monkeypatch, capsys):
    """
    Test to ensure that option W can be set.
    """
    def MyEXE(*args, **kwargs):
        args = list(args)
        args.append([('W ignore', '', 'OPTION')])
        return EXE(*args, **kwargs)

    import PyInstaller.building.build_main
    EXE = PyInstaller.building.build_main.EXE
    monkeypatch.setattr('PyInstaller.building.build_main.EXE', MyEXE)

    pyi_builder.test_source("""
        import sys
        assert 'ignore' in sys.warnoptions
        """)

    _, err = capsys.readouterr()
    assert "'import warnings' failed" not in err


@pytest.mark.parametrize("distutils", [False, True], ids=["sysconfig", "distutils.sysconfig"])
def test_python_makefile(pyi_builder, distutils):
    """
    Tests hooks for ``sysconfig`` and its near-duplicate ``distutils.sysconfig``. Raises an import error if we fail
    to collect the special module that contains the details from pyconfig.h and the Makefile.
    """
    # Ideally we would test that the contents of `sysconfig.get_config_vars()` dict are the same frozen vs. unfrozen,
    # but because some values are paths into a Python installation's guts, these will point into the frozen app when
    # frozen, and therefore not match. Without some fiddly filtering of the paths, this is impossible.

    # As a compromise, test that the dictionary keys are the same to be sure that there is no conditional initialisation
    # of get_config_vars(). I.e., that get_config_vars() does not silently return an empty dictionary if it cannot find
    # the information it needs.
    if distutils:
        from distutils import sysconfig
        import_preamble = 'from distutils '
    else:
        import sysconfig
        import_preamble = ''
    unfrozen_keys = sorted(sysconfig.get_config_vars().keys())

    pyi_builder.test_source(
        f"""
        # The error is raised immediately on import.
        {import_preamble}import sysconfig

        # But just in case, Python later opt for some lazy loading, force
        # configuration retrieval:
        from pprint import pprint
        pprint(sysconfig.get_config_vars())

        unfrozen_keys = {unfrozen_keys}
        assert sorted(sysconfig.get_config_vars()) == unfrozen_keys
        """
    )


def test_set_icon(pyi_builder, data_dir):
    if is_win:
        args = ['--icon', str(data_dir / 'pyi_icon.ico')]
    elif is_darwin:
        # On OS X icon is applied only for windowed mode.
        args = ['--windowed', '--icon', str(data_dir / 'pyi_icon.icns')]
    else:
        pytest.skip('option --icon works only on Windows and macOS')
    os.chdir(os.path.expanduser("~"))
    pyi_builder.test_source("print('Hello Python!')", pyi_args=args)


def test_python_home(pyi_builder):
    pyi_builder.test_script('pyi_python_home.py')


def test_stderr_encoding(tmp_path, pyi_builder):
    # NOTE: '-s' option to pytest disables output capturing, changing this test's result:
    # without -s: pytest process changes its own stdout encoding to 'UTF-8' to capture output. subprocess spawned by
    #             pytest has stdout encoding 'cp1252', which is an ANSI codepage. test fails as they do not match.
    # with -s:    pytest process has stdout encoding from windows terminal, which is an OEM codepage. spawned
    #             subprocess has the same encoding. test passes.

    # Get the current encoding, and save it into file for test frozen application to use.
    # For non-interactive stderr use locale encoding - ANSI codepage. This fixes the test when running with pytest and
    # capturing output.
    encoding = str(sys.stderr.encoding) if sys.stderr.isatty() else locale.getpreferredencoding(False)
    (tmp_path / 'stderr_encoding.build').write_text(encoding, encoding="utf-8")

    pyi_builder.test_script('pyi_stderr_encoding.py')


def test_stdout_encoding(tmp_path, pyi_builder):
    encoding = str(sys.stdout.encoding) if sys.stdout.isatty() else locale.getpreferredencoding(False)
    (tmp_path / 'stdout_encoding.build').write_text(encoding, encoding="utf-8")

    pyi_builder.test_script('pyi_stdout_encoding.py')


def test_site_module_disabled(pyi_builder):
    pyi_builder.test_script('pyi_site_module_disabled.py')


def test_time_module(pyi_builder):
    pyi_builder.test_source("""
        import time
        print(time.strptime(time.ctime()))
        """)


@pytest.mark.darwin
@pytest.mark.linux
def test_time_module_localized(pyi_builder, monkeypatch):
    # This checks that functions 'time.ctime()' and 'time.strptime()' use the same locale. There was an issue with
    # bootloader where every function was using different locale:
    # time.ctime was using 'C'
    # time.strptime was using 'xx_YY' from the environment.
    monkeypatch.setenv('LC_ALL', 'cs_CZ.UTF-8')
    pyi_builder.test_source("""
        import time
        print(time.strptime(time.ctime()))
        """)


# Check that `locale.getlocale()` in frozen application returns user-preferred locale (i.e., that user-preferred
# locale is set in the bootloader during python interpreter setup). The test ensures that the user-preferred locale
# is  set *at all* (see #8305), as well as that it matches the environment variables.
@pytest.mark.darwin
@pytest.mark.linux
def test_user_preferred_locale(pyi_builder):
    # NOTE: this runs the program without arguments, which checks that locale is set at all.
    pyi_builder.test_source(
        """
        import sys
        import locale

        user_locale = locale.getlocale()
        print(f"User locale: {user_locale}", file=sys.stderr)

        if len(sys.argv) == 1:
            # No arguments - check that locale is set at all; the tuple must have two elements, and neither of them is
            # None.
            if (len(user_locale) != 2) or (user_locale[0] is None) or (user_locale[1] is None):
                raise Exception(f"Invalid user locale: {user_locale!r}")
        elif len(sys.argv) == 2:
            expected_locale = tuple(sys.argv[1].split('.'))
            print(f"Expected locale: {expected_locale}", file=sys.stderr)
            if user_locale != expected_locale:
                raise Exception(f"Unexpected user locale: {user_locale!r} (expected: {expected_locale!r})")
        else:
            print(f"Usage: {sys.argv[0]} [expected_locale]")
            sys.exit(1)

        print("OK!", file=sys.stderr)
        """
    )

    # Find executable and run additional tests with locale set via LC_ALL
    exes = pyi_builder._find_executables('test_source')
    assert len(exes) == 1

    test_locales = [
        "en_US.UTF-8",
        "en_US.ISO8859-1",
        "sl_SI.UTF-8",
        "sl_SI.ISO8859-2",
    ]

    for test_locale in test_locales:
        print(f"Running test with locale: {test_locale!r}...", file=sys.stderr)
        env = {
            "LC_ALL": test_locale,
        }
        subprocess.run([exes[0], test_locale], check=True, env=env)


def test_xmldom_module(pyi_builder):
    pyi_builder.test_source(
        """
        print('Importing xml.dom')
        from xml.dom import pulldom
        print('Importing done')
        """
    )


def test_threading_module(pyi_builder):
    pyi_builder.test_source(
        """
        import threading
        import sys

        print('See stderr for messages')
        def print_(*args): print(*args, file=sys.stderr)

        def doit(nm):
            print_(nm, 'started')
            import pyi_testmod_threading
            try:
                print_(nm, pyi_testmod_threading.x)
            finally:
                print_(nm, pyi_testmod_threading)

        t1 = threading.Thread(target=doit, args=('t1',))
        t2 = threading.Thread(target=doit, args=('t2',))
        t1.start()
        t2.start()
        doit('main')
        t1.join() ; print_('t1 joined')
        t2.join() ; print_('t2 joined')
        print_('finished.')
        """
    )


def test_threading_module2(pyi_builder):
    pyi_builder.test_script('pyi_threading_module2.py')


def test_argument(pyi_builder):
    pyi_builder.test_source(
        """
        import sys
        assert sys.argv[1] == "--argument", "sys.argv[1] was %r, expected %r" % (sys.argv[1], "--argument")
        """,
        app_args=["--argument"]
    )


@importorskip('win32com')
def test_pywin32_win32com(pyi_builder):
    pyi_builder.test_source(
        """
        # Test importing some modules from pywin32 package.
        # All modules from pywin32 depens on module pywintypes.
        # This module should be also included.
        import win32com
        import win32com.client
        import win32com.server
        """
    )


#@pytest.mark.xfail(reason="Requires post-create-package hooks (issue #1322)")
@importorskip('win32com')
def test_pywin32_comext(pyi_builder):
    pyi_builder.test_source(
        """
        # Test importing modules from win32com that are actually present in
        # win32comext, and made available by __path__ changes in win32com.
        from win32com.shell import shell
        from win32com.propsys import propsys
        from win32com.bits import bits
        """
    )


@importorskip('win32ui')
@xfail(reason="https://github.com/mhammond/pywin32/issues/1614")
def test_pywin32_win32ui(pyi_builder):
    pyi_builder.test_source(
        """
        # Test importing some modules from pywin32 package.
        # All modules from pywin32 depens on module pywintypes.
        # This module should be also included.
        import win32ui
        from pywin.mfc.dialog import Dialog
        d = Dialog(win32ui.IDD_SIMPLE_INPUT)
        """
    )


@pytest.mark.win32
def test_renamed_exe(pyi_builder):
    _old_find_executables = pyi_builder._find_executables

    def _find_executables(name):
        old_executables = _old_find_executables(name)
        new_executables = []
        for old_exe in old_executables:
            old_exe_path = pathlib.Path(old_exe)
            new_exe_path = old_exe_path.with_name(f"renamed_{old_exe_path.name}")
            old_exe_path.rename(new_exe_path)

            new_executables.append(str(new_exe_path))
        return new_executables

    pyi_builder._find_executables = _find_executables
    pyi_builder.test_source("print('Hello Python!')")


def test_spec_with_utf8(pyi_builder_spec):
    pyi_builder_spec.test_spec('spec-with-utf8.spec')


@pytest.mark.darwin
def test_osx_override_info_plist(pyi_builder_spec):
    pyi_builder_spec.test_spec('pyi_osx_override_info_plist.spec')


def test_hook_collect_submodules(pyi_builder, script_dir):
    # This is designed to test the operation of PyInstaller.utils.hook.collect_submodules. To do so:
    #
    # 1. It imports the dummy module pyi_collect_submodules_mod, which contains nothing.
    # 2. This causes hook-pyi_collect_submodules_mod.py to be run, which collects some dummy submodules. In this case,
    #    it collects from modules/pyi_testmod_relimp.
    # 3. Therefore, we should be able to find hidden imports under pyi_testmod_relimp.
    pyi_builder.test_source(
        """
        import pyi_collect_submodules_mod
        __import__('pyi_testmod_relimp.B.C')
        """,
        pyi_args=['--additional-hooks-dir', str(script_dir / 'pyi_hooks')]
    )


# Test that PyInstaller can handle a script with an arbitrary extension.
def test_arbitrary_ext(pyi_builder):
    pyi_builder.test_script('pyi_arbitrary_ext.foo')


def test_option_runtime_tmpdir(pyi_builder):
    """
    Test to ensure that option `runtime_tmpdir` can be set and has effect.
    """

    pyi_builder.test_source(
        """
        print('test - runtime_tmpdir - custom runtime temporary directory')
        import os
        import sys

        cwd = os.path.abspath(os.getcwd())
        runtime_tmpdir = os.path.abspath(sys._MEIPASS)
        # for onedir mode, runtime_tmpdir == cwd
        # for onefile mode, os.path.dirname(runtime_tmpdir) == cwd
        if not runtime_tmpdir == cwd and not os.path.dirname(runtime_tmpdir) == cwd:
            raise SystemExit('Expected sys._MEIPASS to be under current working dir.'
                             ' sys._MEIPASS = ' + runtime_tmpdir + ', cwd = ' + cwd)
        print('test - done')
        """,
        # Set runtime-tmpdir to current working dir
        pyi_args=['--runtime-tmpdir', '.']
    )


@xfail(reason='Issue #3037 - all scripts share the same global vars')
def test_several_scripts1(pyi_builder_spec):
    """
    Verify each script has it's own global vars (original case, see issue #2949).
    """
    pyi_builder_spec.test_spec('several-scripts1.spec')


@xfail(reason='Issue #3037 - all scripts share the same global vars')
def test_several_scripts2(pyi_builder_spec):
    """
    Verify each script has it's own global vars (basic test).
    """
    pyi_builder_spec.test_spec('several-scripts2.spec')


def test_hyphenated_hiddenimport(pyi_builder):
    """
    Verify that a spec whose hiddenimports include a hyphenated module name is valid
    See issue #8591
    """
    pyi_builder.test_source(
        """
        print("hello!")
        """, pyi_args=['--hiddenimport', 'fake-hyphenated-module']
    )


@pytest.mark.win32
def test_pe_checksum(pyi_builder):
    import ctypes
    from ctypes import wintypes

    pyi_builder.test_source("print('hello')")
    exes = pyi_builder._find_executables('test_source')
    assert exes
    for exe in exes:
        # Validate the PE checksum using the official Windows API for doing so.
        # https://docs.microsoft.com/en-us/windows/win32/api/imagehlp/nf-imagehlp-mapfileandchecksumw
        header_sum = wintypes.DWORD()
        checksum = wintypes.DWORD()
        assert ctypes.windll.imagehlp.MapFileAndCheckSumW(
            ctypes.c_wchar_p(exe), ctypes.byref(header_sum), ctypes.byref(checksum)
        ) == 0

        assert header_sum.value == checksum.value


def test_onefile_longpath(pyi_builder, tmp_path):
    """
    Verify that files with paths longer than 260 characters are correctly extracted from the onefile build.
    See issue #5615."
    """
    # The test is relevant only for onefile builds
    if pyi_builder._mode != 'onefile':
        pytest.skip('The test is relevant only to onefile builds.')
    # Create data file with secret
    _SECRET = 'LongDataPath'
    src_path = tmp_path / 'data.txt'
    src_path.write_text(_SECRET, encoding='utf-8')
    # Generate long target filename/path; eight equivalents of SHA256 strings plus data.txt should push just the
    # _MEIPASS-relative path beyond 260 characters...
    dst_filename = os.path.join(*[32 * chr(c) for c in range(ord('A'), ord('A') + 8)], 'data.txt')
    assert len(dst_filename) >= 260
    # Name for --add-data
    add_data_arg = f"{src_path}:{os.path.dirname(dst_filename)}"
    pyi_builder.test_source(
        f"""
        import sys
        import os

        data_file = os.path.join(sys._MEIPASS, {str(dst_filename)!r})
        print("Reading secret from %r" % (data_file))
        with open(data_file, 'r') as fp:
            secret = fp.read()
        assert secret == {_SECRET!r}
        """,
        pyi_args=['--add-data', add_data_arg]
    )


@pytest.mark.win32
@pytest.mark.parametrize("icon", ["icon_default", "icon_none", "icon_given"])
def test_application_executable_has_manifest(pyi_builder, icon):
    """
    Verify that builds on Windows end up having manifest embedded. See issue #5624.
    This test was initially limited only to onefile builds, but as we now always embed manifest into executable, it
    now covers both builds.
    """
    from PyInstaller.utils.win32 import winmanifest
    from PyInstaller import PACKAGEPATH

    # Icon type
    if icon == 'icon_default':
        # Default; no --icon argument
        extra_args = []
    elif icon == 'icon_none':
        # Disable icon completely; --icon NONE
        extra_args = ['--icon', 'NONE']
    elif icon == 'icon_given':
        # Locate pyinstaller's default icon, and explicitly give it
        # via --icon argument
        icon_path = os.path.join(PACKAGEPATH, 'bootloader', 'images', 'icon-console.ico')
        extra_args = ['--icon', icon_path]
    # Build the executable...
    pyi_builder.test_source("""print('Hello world!')""", pyi_args=extra_args)
    # ... and ensure that it contains manifest
    exes = pyi_builder._find_executables('test_source')
    assert exes
    for exe in exes:
        manifest = winmanifest.read_manifest_from_executable(exe)
        assert manifest, "No manifest resources found!"


@pytest.mark.parametrize("append_pkg", [True, False], ids=["embedded", "sideload"])
def test_sys_executable(pyi_builder, append_pkg, monkeypatch):
    """
    Verify that sys.executable points to the executable, regardless of build mode (onedir vs. onefile) and the
    append_pkg setting (embedded vs. side-loaded CArchive PKG).
    """
    # Set append_pkg; taken from test_pyz_as_external_file
    import PyInstaller.building.build_main
    EXE = PyInstaller.building.build_main.EXE

    def MyEXE(*args, **kwargs):
        kwargs['append_pkg'] = append_pkg
        return EXE(*args, **kwargs)

    monkeypatch.setattr('PyInstaller.building.build_main.EXE', MyEXE)

    # Expected executable basename
    exe_basename = 'test_source'
    if is_win:
        exe_basename += '.exe'

    pyi_builder.test_source(
        f"""
        import sys
        import os
        exe_basename = os.path.basename(sys.executable)
        assert exe_basename == {exe_basename!r}, "Unexpected basename(sys.executable): " + exe_basename
        """
    )


@pytest.mark.win32
def test_subprocess_in_windowed_mode(pyi_windowed_builder):
    """Test invoking subprocesses from a PyInstaller app built in windowed mode."""

    pyi_windowed_builder.test_source(
        fr"""
        from subprocess import PIPE, run
        from unittest import TestCase

        # Lazily use unittest's rich assertEqual() for assertions with builtin diagnostics.
        assert_equal = TestCase().assertEqual

        # Path to python interpreter
        python = {sys.executable!r}

        # Run with empty command to ensure interpreter works.
        run([python, "-c", ""], check=True)

        # Verify that stdin, stdout and stderr still work and haven't been muddled.
        p = run([python, "-c", "print('foo')"], stdout=PIPE, universal_newlines=True)
        assert_equal(p.stdout, "foo\n", p.stdout)

        p = run([python, "-c", r"import sys; sys.stderr.write('bar\n')"], stderr=PIPE, universal_newlines=True)
        assert_equal(p.stderr, "bar\n", p.stderr)

        p = run([python], input="print('foo')\nprint('bar')\n", stdout=PIPE, universal_newlines=True)
        assert_equal(p.stdout, "foo\nbar\n", p.stdout)
        """,
        pyi_args=["--windowed"]
    )


def test_package_entry_point_name_collision(pyi_builder):
    """
    Check when an imported package has the same name as the entry point script. Despite the obvious ambiguity, Python
    still handles this case fine and PyInstaller should too.
    """
    script = pathlib.Path(__file__).parent / 'data' / 'name_clash_with_entry_point' / 'matching_name.py'

    # Each file prints its filename and the value of __name__.
    expected = [
        ('matching_name.py', '__main__'),
        ('matching_name/__init__.py', 'matching_name'),
        ('matching_name/submodule.py', 'matching_name.submodule'),
    ]

    # Include a verification that unfrozen Python does still work.
    p = subprocess.run([sys.executable, str(script)], stdout=subprocess.PIPE, encoding="utf-8")
    assert re.findall("Running (.*) as (.*)", p.stdout) == expected

    pyi_builder.test_script(str(script))
    exe, = pyi_builder._find_executables("matching_name")
    p = subprocess.run([exe], stdout=subprocess.PIPE, encoding="utf-8")
    assert re.findall("Running (.*) as (.*)", p.stdout) == expected


def test_contents_directory(pyi_builder):
    """
    Test the --contents-directory option, including changing it without --clean.
    """
    if pyi_builder._mode != 'onedir':
        pytest.skip('--contents-directory does not affect onefile builds.')

    pyi_builder.test_source("", pyi_args=["--contents-directory", "foo"])
    exe, = pyi_builder._find_executables("test_source")
    bundle = pathlib.Path(exe).parent
    assert (bundle / "foo").is_dir()

    pyi_builder.test_source("", pyi_args=["--contents-directory", "é³þ³źć🚀", "--noconfirm"])
    assert not (bundle / "foo").exists()
    assert (bundle / "é³þ³źć🚀").is_dir()

    with pytest.raises(SystemExit, match='Invalid value "\\.\\." passed'):
        pyi_builder.test_source("", pyi_args=["--contents-directory", "..", "--noconfirm"])


def test_legacy_onedir_layout(pyi_builder):
    """
    Test the --contents-directory=., which re-enables the legacy onedir layout.
    """
    if pyi_builder._mode != 'onedir':
        pytest.skip('--contents-directory does not affect onefile builds.')

    pyi_builder.test_source(
        """
        import sys
        import os

        # NOTE: the paths set by bootloader (`sys._MEIPASS`, `__file__`) may end up using different separator than
        # paths set by the python interpreter itself (e.g., `sys.executable`) - for example, under msys2/mingw
        # python on Windows). Therefore, we normalize the separator via `os.path.normpath` before comparison.
        assert os.path.normpath(sys._MEIPASS) == os.path.dirname(sys.executable)
        assert os.path.normpath(os.path.dirname(__file__)) == os.path.dirname(sys.executable)
        """,
        pyi_args=["--contents-directory", "."]
    )


def test_spec_options(pyi_builder, spec_dir, capsys):
    if pyi_builder._mode != 'onedir':
        pytest.skip('spec file is onedir mode only')

    pyi_builder.test_spec(
        spec_dir / "pyi_spec_options.spec",
        pyi_args=["--", "--optional-dependency", "email", "--optional-dependency", "gzip"]
    )
    exe, = pyi_builder._find_executables("pyi_spec_options")
    p = subprocess.run([exe], stdout=subprocess.PIPE, encoding="utf-8")
    assert p.stdout == "Available dependencies: email gzip\n"

    capsys.readouterr()
    with pytest.raises(SystemExit) as ex:
        pyi_builder.test_spec(spec_dir / "pyi_spec_options.spec", pyi_args=["--", "--help"])
    assert ex.value.code == 0
    assert "help blah blah blah" in capsys.readouterr().out

    with pytest.raises(SystemExit) as ex:
        pyi_builder.test_spec(spec_dir / "pyi_spec_options.spec", pyi_args=["--", "--onefile"])
    assert "pyi_spec_options.spec: error: unrecognized arguments: --onefile" in capsys.readouterr().err
