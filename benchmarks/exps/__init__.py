# -*- coding: future_fstrings -*-
#
# Copyright 2019 Peifeng Yu <peifeng@umich.edu>
# 
# This file is part of Salus
# (see https://github.com/SymbioticLab/Salus).
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#    http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
from __future__ import absolute_import, print_function, division, unicode_literals

import time
import re
import logging
from absl import flags
from typing import Union, Iterable, List, TypeVar, Callable

import benchmarks.driver.utils.prompt as prompt
from benchmarks.driver.runner import Executor
from benchmarks.driver.server.config import presets
from benchmarks.driver.server import SalusServer, SalusConfig
from benchmarks.driver.tfserver import TFDistServer
from benchmarks.driver.utils import atomic_directory, try_with_default, UsageError, kill_tree
from benchmarks.driver.utils.compatiblity import pathlib
from benchmarks.driver.workload import Workload, WTL, ResourceGeometry

Path = pathlib.Path
T = TypeVar('T')
logger = logging.getLogger(__name__)
FLAGS = flags.FLAGS

flags.DEFINE_boolean('ignore_error', False, 'Ignore error on workload')


class Pause(int):
    """Represent a pause in an action sequence"""
    Manual = None  # type: Pause
    Wait = None  # type: Pause

    def run(self, workloads, **kwargs):
        if self == Pause.Manual:
            prompt.pause()
        elif self == Pause.Wait:
            logger.info(f"Waiting current {len(workloads)} workloads to finish")
            SalusServer.wait_workloads(workloads)
        else:
            logger.info(f"Sleep {self} seconds")
            time.sleep(self)


Pause.Manual = Pause(-1)
"""Pause execution and wait for human input"""

Pause.Wait = Pause(-2)
"""Wait until previous workloads finishes"""


class RunFn(object):
    """Represent arbitrary function in an action sequence"""
    __slots__ = '_fn'

    def __init__(self, fn):
        # type: (Callable[Iterable[Workload], None]) -> None
        self._fn = fn

    def run(self, workloads, **kwargs):
        return self._fn(workloads, **kwargs)


TAction = Union[Pause, RunFn, Workload]


def run_tf(output_dir, *actions):
    # type: (Path, *TAction) -> List[Workload]
    """Run a sequence of actions"""
    workloads = []  # type: List[Workload]

    try:
        with atomic_directory(output_dir) as temp_dir:  # type: Path
            # Do action specified in seq
            for act in actions:
                if isinstance(act, Workload):
                    if act.executor != Executor.TF:
                        raise ValueError('run_tf can only run TF workloads')
                    output_file = temp_dir / f'{act.output_name}.{act.batch_num}iter.{len(workloads)}.output'

                    act.run(output_file)
                    workloads.append(act)
                elif isinstance(act, (Pause, RunFn)):
                    act.run(workloads, temp_dir=temp_dir)
                else:
                    raise ValueError(f"Unexpected value `{act}' of {type(act)} passed to run_seq")

            logger.info(f'Waiting all workloads to finish')
            SalusServer.wait_workloads(workloads)
    except Exception:
        logger.exception("Got exception when running workloads")
    finally:
        # if there's alive, we are doing cleanup
        for w in workloads:
            if w.proc is not None and w.proc.poll() is None:
                logger.warning(f'Killing workload that is not stopped yet: {w.canonical_name}')
                kill_tree(w.proc, hard=True)

        # check each workloads and fix workload output_file path
        for w in workloads:
            if not FLAGS.ignore_error and w.proc.returncode != 0:
                prompt.pause()
                raise RuntimeError(f'Workload {w.canonical_name} did not finish cleanly: {w.proc.returncode}')
            w.output_file = output_dir / w.output_file.name

    return workloads


def run_seq(scfg, *actions):
    # type: (SalusConfig, *TAction) -> List[Workload]
    """Run a sequence of actions"""
    workloads = []  # type: List[Workload]

    try:
        with atomic_directory(scfg.output_dir) as temp_dir:  # type: Path
            # start server
            ss = SalusServer(scfg.copy(output_dir=temp_dir))
            with ss.run():
                # Do action specified in seq
                for act in actions:
                    ss.check()

                    if isinstance(act, Workload):
                        output_file = temp_dir / f'{act.output_name}.{act.batch_num}iter.{len(workloads)}.output'

                        act.run(output_file)
                        workloads.append(act)
                    elif isinstance(act, (Pause, RunFn)):
                        act.run(workloads, temp_dir=temp_dir)
                    else:
                        raise ValueError(f"Unexpected value `{act}' of {type(act)} passed to run_seq")

                logger.info(f'Waiting all workloads to finish')
                ss.wait_workloads(workloads)
    finally:
        # if there's alive, we are doing cleanup
        for w in workloads:
            if w.proc is not None and w.proc.poll() is None:
                logger.warning(f'Killing workload that is not stopped yet: {w.canonical_name}')
                kill_tree(w.proc, hard=True)

        # check each workloads and fix workload output_file path
        for w in workloads:
            if not FLAGS.ignore_error and w.proc.returncode != 0:
                prompt.pause()
                raise RuntimeError(f'Workload {w.canonical_name} did not finish cleanly: {w.proc.returncode}')
            w.output_file = scfg.output_dir / w.output_file.name
    return workloads


def run_tfdist(output, *actions):
    # type: (Path, *TAction) -> List[Workload]
    """Run a sequence of actions"""
    workloads = []  # type: List[Workload]

    try:
        with atomic_directory(output) as temp_dir:  # type: Path
            # start server
            ss = TFDistServer(outputdir=temp_dir)
            with ss.run():
                # Do action specified in seq
                for act in actions:
                    ss.check()

                    if isinstance(act, Workload):
                        if act.executor != Executor.TFDist:
                            raise ValueError('run_tfdist can only run TFDist workloads')
                        output_file = temp_dir / f'{act.output_name}.{act.batch_num}iter.{len(workloads)}.output'

                        act.run(output_file)
                        workloads.append(act)
                    elif isinstance(act, (Pause, RunFn)):
                        act.run(workloads, temp_dir=temp_dir)
                    else:
                        raise ValueError(f"Unexpected value `{act}' of {type(act)} passed to run_tfdist")

                logger.info(f'Waiting all workloads to finish')
                ss.wait_workloads(workloads)
    finally:
        # if there's alive, we are doing cleanup
        for w in workloads:
            if w.proc is not None and w.proc.poll() is None:
                logger.warning(f'Killing workload that is not stopped yet: {w.canonical_name}')
                kill_tree(w.proc, hard=True)

        # check each workloads and fix workload output_file path
        for w in workloads:
            if not FLAGS.ignore_error and w.proc.returncode != 0:
                prompt.pause()
                raise RuntimeError(f'Workload {w.canonical_name} did not finish cleanly: {w.proc.returncode}')
            w.output_file = output
    return workloads


def parse_actions_from_cmd(argv):
    # type: (Iterable[str]) -> List[TAction]
    """Parse actions from command line
    E.g.
    manual inception3 75(batch_size) 128(batch_num) 5(wait) manual
    """
    actions = []
    while argv:
        curr = argv[0]
        if curr == 'manual':
            actions.append(Pause.Manual)
            argv.pop(0)
            continue
        try:
            name, batch_size, batch_num, pause = argv[:4]
            argv = argv[4:]
            actions.append(WTL.create(name, batch_size, batch_num))
        except ValueError:
            raise UsageError(f'Unexpected sequence of arguments: {argv}')

    return actions


def maybe_forced_preset(default):
    # type: (Callable[[], SalusConfig]) -> SalusConfig
    """Maybe return forced preset"""
    if FLAGS.force_preset:
        logger.info(f'Using server config preset: {FLAGS.force_preset}')
        return getattr(presets, FLAGS.force_preset)()
    logger.info(f'Using server config preset: {default.__name__}')
    return default()


def parse_output_float(outputfile, pattern, group=1):
    """Parse outputfile using pattern"""
    if not outputfile.exists():
        msg = f'File not found after running: {outputfile}'
        logger.fatal(msg)
        raise ValueError(msg)

    ptn = re.compile(pattern)
    with outputfile.open() as f:
        for line in f:
            line = line.rstrip()
            m = ptn.match(line)
            if m:
                try:
                    return float(m.group(group))
                except (ValueError, IndexError):
                    continue
    raise ValueError(f"Pattern `{pattern}' not found in output file {outputfile}")


def case_switch_main(fn):
    """Turn a function returning a list of functions in to a main function that accepts names to choose the name"""
    cases = fn()
    if len(cases) == 0:
        return lambda argv: None

    dispatch = {
        f.__name__: f
        for f in cases
    }
    default = cases[0].__name__

    def main(argv):
        command = argv[0] if argv else default
        return dispatch[command](argv[1:])

    return main


def update_jct(workload, update_global=False):
    # type: (Workload, bool) -> None
    """Parse and update JCT value of a completed workload"""
    if workload.proc is None or workload.proc.returncode != 0:
        raise ValueError(f'Workload {workload.name} not started or terminated in error')

    jct = parse_output_float(workload.output_file, r'^JCT: ([0-9.]+) .*')
    workload.geometry.jct = jct
    if update_global:
        WTL.from_name(workload.name).add_geometry(workload.rcfg, workload.executor, ResourceGeometry(jct=jct))
