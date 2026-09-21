"""Run the existing audit against a read-only program from the analyzed project."""
import os
import runpy
from pathlib import Path

import pyghidra

pyghidra.start()
from ghidra.base.project import GhidraProject
from ghidra.program.flatapi import FlatProgramAPI
from ghidra.util.task import ConsoleTaskMonitor
from java.lang import Object

project = GhidraProject.openProject(os.environ['REMIX_GHIDRA_PROJECT_DIR'], 'BethesdaGhidraScripts', True)
consumer = Object()
program = None
try:
    domain_file = project.getProject().getProjectData().getFile('/skyrim/ae1.7/SkyrimSE.exe.unpacked.exe')
    monitor = ConsoleTaskMonitor()
    program = domain_file.getReadOnlyDomainObject(consumer, -1, monitor)
    api = FlatProgramAPI(program, monitor)
    context = {'currentProgram': program, 'monitor': monitor}
    for name in ('getBytes', 'getFunctionAt', 'getFunctionContaining'):
        context[name] = getattr(api, name)
    runpy.run_path(str(Path(__file__).with_name('RenderBoundaryAudit.py')), init_globals=context)
finally:
    if program is not None:
        program.release(consumer)
    project.close()
