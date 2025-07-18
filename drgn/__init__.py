# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: LGPL-2.1-or-later

"""
Programmable debugger

drgn is a programmable debugger. It is built on top of Python, so if you
don't know at least a little bit of Python, go learn it first.

drgn supports an interactive mode and a script mode. Both are simply a
Python interpreter initialized with a special drgn.Program object named
"prog" that represents the program which is being debugged.

In interactive mode, try

>>> help(prog)

or

>>> help(drgn.Program)

to learn more about how to use it.

Objects in the program (e.g., variables and values) are represented by
drgn.Object. Try

>>> help(drgn.Object)

Types are represented by drgn.Type objects. Try

>>> help(drgn.Type)

Various helpers are provided for particular types of programs. Try

>>> import drgn.helpers
>>> help(drgn.helpers)

The drgn.internal package contains the drgn internals. Everything in
that package should be considered implementation details and should not
be used.
"""

import io
import pkgutil
import sys
import types
from typing import Any, Dict, Optional, Union

from _drgn import (
    NULL,
    AbsenceReason,
    Architecture,
    DebugInfoOptions,
    ExtraModule,
    FaultError,
    FindObjectFlags,
    IntegerLike,
    KmodSearchMethod,
    Language,
    MainModule,
    MissingDebugInfoError,
    Module,
    ModuleFileStatus,
    NoDefaultProgramError,
    Object,
    ObjectAbsentError,
    ObjectNotFoundError,
    OutOfBoundsError,
    Path,
    Platform,
    PlatformFlags,
    PrimitiveType,
    Program,
    ProgramFlags,
    Qualifiers,
    Register,
    RelocatableModule,
    SharedLibraryModule,
    StackFrame,
    StackTrace,
    SupplementaryFileKind,
    Symbol,
    SymbolBinding,
    SymbolIndex,
    SymbolKind,
    Thread,
    Type,
    TypeEnumerator,
    TypeKind,
    TypeKindSet,
    TypeMember,
    TypeParameter,
    TypeTemplateParameter,
    VdsoModule,
    WantedSupplementaryFile,
    alignof,
    cast,
    container_of,
    filename_matches,
    get_default_prog,
    host_platform,
    implicit_convert,
    offsetof,
    program_from_core_dump,
    program_from_kernel,
    program_from_pid,
    reinterpret,
    set_default_prog,
    sizeof,
)

# flake8 doesn't honor import X as X. See PyCQA/pyflakes#474.
# isort: split
from _drgn import (  # noqa: F401
    _elfutils_version as _elfutils_version,
    _enable_dlopen_debuginfod as _enable_dlopen_debuginfod,
    _have_debuginfod as _have_debuginfod,
    _with_libkdumpfile as _with_libkdumpfile,
    _with_lzma as _with_lzma,
)
from drgn.internal.version import __version__ as __version__  # noqa: F401

__all__ = (
    "AbsenceReason",
    "Architecture",
    "DebugInfoOptions",
    "ExtraModule",
    "FaultError",
    "FindObjectFlags",
    "IntegerLike",
    "KmodSearchMethod",
    "Language",
    "MainModule",
    "MissingDebugInfoError",
    "Module",
    "ModuleFileStatus",
    "NULL",
    "NoDefaultProgramError",
    "Object",
    "ObjectAbsentError",
    "ObjectNotFoundError",
    "OutOfBoundsError",
    "Path",
    "Platform",
    "PlatformFlags",
    "PrimitiveType",
    "Program",
    "ProgramFlags",
    "Qualifiers",
    "Register",
    "RelocatableModule",
    "SharedLibraryModule",
    "StackFrame",
    "StackTrace",
    "SupplementaryFileKind",
    "Symbol",
    "SymbolBinding",
    "SymbolIndex",
    "SymbolKind",
    "Thread",
    "Type",
    "TypeEnumerator",
    "TypeKind",
    "TypeKindSet",
    "TypeMember",
    "TypeParameter",
    "TypeTemplateParameter",
    "VdsoModule",
    "WantedSupplementaryFile",
    "alignof",
    "cast",
    "container_of",
    "execscript",
    "filename_matches",
    "get_default_prog",
    "host_platform",
    "implicit_convert",
    "offsetof",
    "program_from_core_dump",
    "program_from_kernel",
    "program_from_pid",
    "reinterpret",
    "set_default_prog",
    "sizeof",
    "stack_trace",
)


# From https://docs.python.org/3/reference/import.html#import-related-module-attributes.
_special_globals = frozenset(
    [
        "__name__",
        "__loader__",
        "__package__",
        "__spec__",
        "__path__",
        "__file__",
        "__cached__",
    ]
)


def execscript(path: str, *args: str, globals: Optional[Dict[str, Any]] = None) -> None:
    """
    Execute a script.

    The script is executed in the same context as the caller: currently defined
    globals are available to the script, and globals defined by the script are
    added back to the calling context.

    This is most useful for executing scripts from interactive mode. For
    example, you could have a script named ``exe.py``:

    .. code-block:: python3

        \"\"\"Get all tasks executing a given file.\"\"\"

        import sys

        from drgn.helpers.linux.fs import d_path
        from drgn.helpers.linux.pid import find_task

        def task_exe_path(task):
            if task.mm:
                return d_path(task.mm.exe_file.f_path).decode()
            else:
                return None

        tasks = [
            task for task in for_each_task()
            if task_exe_path(task) == sys.argv[1]
        ]

    Then, you could execute it and use the defined variables and functions:

    >>> execscript('exe.py', '/usr/bin/bash')
    >>> tasks[0].pid
    (pid_t)358442
    >>> task_exe_path(find_task(357954))
    '/usr/bin/vim'

    :param path: File path of the script.
    :param args: Zero or more additional arguments to pass to the script. This
        is a :ref:`variable argument list <python:tut-arbitraryargs>`.
    :param globals: If provided, globals to use instead of the caller's.
    """
    # This is based on runpy.run_path(), which we can't use because we want to
    # update globals even if the script throws an exception.
    saved_module = []
    try:
        saved_module.append(sys.modules["__main__"])
    except KeyError:
        pass
    saved_argv = sys.argv
    try:
        module = types.ModuleType("__main__")
        sys.modules["__main__"] = module
        sys.argv = [path]
        sys.argv.extend(args)

        with io.open_code(path) as f:
            code = pkgutil.read_code(f)
        if code is None:
            with io.open_code(path) as f:
                code = compile(f.read(), path, "exec")
        module.__spec__ = None
        module.__file__ = path
        module.__cached__ = None  # type: ignore[attr-defined]

        if globals is not None:
            caller_globals = globals
        else:
            caller_globals = sys._getframe(1).f_globals
        caller_special_globals = {
            name: caller_globals[name]
            for name in _special_globals
            if name in caller_globals
        }
        for name, value in caller_globals.items():
            if name not in _special_globals:
                setattr(module, name, value)

        try:
            exec(code, vars(module))
        finally:
            caller_globals.clear()
            caller_globals.update(caller_special_globals)
            for name, value in vars(module).items():
                if name not in _special_globals:
                    caller_globals[name] = value
    finally:
        sys.argv = saved_argv
        if saved_module:
            sys.modules["__main__"] = saved_module[0]
        else:
            del sys.modules["__main__"]


def stack_trace(thread: Union[Object, IntegerLike]) -> StackTrace:
    """
    Get the stack trace for the given thread using the :ref:`default program
    argument <default-program>`.

    See :meth:`Program.stack_trace()` for more details.

    :param thread: Thread ID, ``struct pt_regs`` object, or
        ``struct task_struct *`` object.
    """
    if isinstance(thread, Object):
        return thread.prog_.stack_trace(thread)
    else:
        return get_default_prog().stack_trace(thread)
