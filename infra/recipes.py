#!/usr/bin/env python3
# Copyright 2019 The LUCI Authors. All rights reserved.
# Use of this source code is governed under the Apache License, Version 2.0
# that can be found in the LICENSE file.

# We want to run python in unbuffered mode; however shebangs on linux grab the
# entire rest of the shebang line as a single argument, leading to errors like:
#
#   /usr/bin/env: 'python3 -u': No such file or directory
#
# This little shell hack is a triple-quoted noop in python, but in sh it
# evaluates to re-exec'ing this script in unbuffered mode.
# pylint: disable=pointless-string-statement
''''exec python3 -u -- "$0" ${1+"$@"} # '''
# vi: syntax=python
"""Bootstrap script to clone and forward to the recipe engine tool.

*******************
** DO NOT MODIFY **
*******************

This is a copy of https://chromium.googlesource.com/infra/luci/recipes-py/+/main/recipes.py.
To fix bugs, fix in the googlesource repo then run the autoroller.
"""

# pylint: disable=wrong-import-position
import argparse
import errno
import json
import logging
import os
import shutil
import subprocess
import sys
import urllib.parse as urlparse

from collections import namedtuple
from io import open  # pylint: disable=redefined-builtin
from typing import Tuple, Union, List, Optional


# The dependency entry for the recipe_engine in the client repo's recipes.cfg
#
# url (str) - the url to the engine repo we want to use.
# revision (str) - the git revision for the engine to get.
# branch (str) - the branch to fetch for the engine as an absolute ref (e.g.
#   refs/heads/main)
EngineDep = namedtuple('EngineDep', 'url revision branch')


class MalformedRecipesCfg(Exception):

  def __init__(self, msg: str, path: str):
    full_message = 'malformed recipes.cfg: %s: %r' % (msg, path)
    super(MalformedRecipesCfg, self).__init__(full_message)


def parse(repo_root: str,
          recipes_cfg_path: str) -> Tuple[Union[EngineDep, None], str, bool]:
  """Parse is a lightweight a recipes.cfg file parser.

    Args:
      repo_root (str) - native path to the root of the repo we're trying to run
        recipes for.
      recipes_cfg_path (str) - native path to the recipes.cfg file to process.

    Returns (as tuple):
      engine_dep (EngineDep|None): The recipe_engine dependency, or None, if the
        current repo IS the recipe_engine.
      recipes_path (str) - native path to where the recipes live inside of the
        current repo (i.e. the folder containing `recipes/` and/or
        `recipe_modules`)
      py3_only (bool) - True if this repo has been marked as ONLY supporting
        python3.
    """
  with open(recipes_cfg_path, 'r') as fh:
    pb = json.load(fh)
  py3_only = pb.get('py3_only', False)

  try:
    if pb['api_version'] != 2:
      raise MalformedRecipesCfg('unknown version %d' % pb['api_version'],
                                recipes_cfg_path)

    # If we're running ./recipes.py from the recipe_engine repo itself, then
    # return None to signal that there's no EngineDep.
    repo_name = pb.get('repo_name')
    if not repo_name:
      repo_name = pb['project_id']
    if repo_name == 'recipe_engine':
      return None, pb.get('recipes_path', ''), py3_only

    engine = pb['deps']['recipe_engine']

    if 'url' not in engine:
      raise MalformedRecipesCfg(
          'Required field "url" in dependency "recipe_engine" not found',
          recipes_cfg_path)

    engine.setdefault('revision', '')
    engine.setdefault('branch', 'refs/heads/main')
    recipes_path = pb.get('recipes_path', '')

    # TODO(iannucci): only support absolute refs
    if not engine['branch'].startswith('refs/'):
      engine['branch'] = 'refs/heads/' + engine['branch']

    recipes_path = os.path.join(repo_root,
                                recipes_path.replace('/', os.path.sep))
    return EngineDep(**engine), recipes_path, py3_only
  except KeyError as ex:
    raise MalformedRecipesCfg(str(ex), recipes_cfg_path)


IS_WIN = sys.platform.startswith(('win', 'cygwin'))

_BAT = '.bat' if IS_WIN else ''
GIT = 'git' + _BAT
CIPD = 'cipd' + _BAT
REQUIRED_BINARIES = {GIT, CIPD}


def _is_executable(path: str) -> bool:
  return os.path.isfile(path) and os.access(path, os.X_OK)


def _is_on_path(basename: str) -> bool:
  return shutil.which(basename) is not None


def _subprocess_call(argv: List[str], **kwargs) -> int:
  logging.info('Running %r', argv)
  return subprocess.call(argv, **kwargs)


def _git_check_call(argv: List[str], **kwargs) -> None:
  argv = [GIT] + argv
  logging.info('Running %r', argv)
  subprocess.check_call(argv, **kwargs)


def _git_output(argv: List[str], **kwargs) -> bytes:
  argv = [GIT] + argv
  logging.info('Running %r', argv)
  return subprocess.check_output(argv, **kwargs)


def parse_args(argv: List[str]) -> Tuple[Optional[str], str]:
  """This extracts a subset of the arguments that this bootstrap script cares
    about. Currently this consists of:
      * an override for the recipe engine in the form of `-O recipe_engine=/path`
      * the --package option.
    """
  PREFIX = 'recipe_engine='

  p = argparse.ArgumentParser(add_help=False)
  p.add_argument('-O', '--project-override', action='append')
  p.add_argument('--package', type=os.path.abspath)
  args, _ = p.parse_known_args(argv)
  for override in args.project_override or ():
    if override.startswith(PREFIX):
      return override[len(PREFIX):], args.package
  return None, args.package


def checkout_engine(engine_path: str | None, repo_root: str,
                    recipes_cfg_path: str) -> Tuple[str, bool]:
  """Checks out the recipe_engine repo pinned in recipes.cfg.

    Returns the path to the recipe engine repo and the py3_only boolean.
    """
  dep, recipes_path, py3_only = parse(repo_root, recipes_cfg_path)
  if dep is None:
    # we're running from the engine repo already!
    return os.path.join(repo_root, recipes_path), py3_only

  url = dep.url

  if not engine_path and url.startswith('file://'):
    engine_path = urlparse.urlparse(url).path

  if not engine_path:
    revision = dep.revision
    branch = dep.branch

    # Ensure that we have the recipe engine cloned.
    engine_path = os.path.join(recipes_path, '.recipe_deps', 'recipe_engine')

    with open(os.devnull, 'w') as NUL:
      # Note: this logic mirrors the logic in recipe_engine/fetch.py
      _git_check_call(['init', engine_path], stdout=NUL)

      try:
        _git_check_call(['rev-parse', '--verify',
                         '%s^{commit}' % revision],
                        cwd=engine_path,
                        stdout=NUL,
                        stderr=NUL)
      except subprocess.CalledProcessError:
        _git_check_call(['fetch', '--quiet', url, branch],
                        cwd=engine_path,
                        stdout=NUL)

    try:
      _git_check_call(['diff', '--quiet', revision], cwd=engine_path)
    except subprocess.CalledProcessError:
      index_lock = os.path.join(engine_path, '.git', 'index.lock')
      try:
        os.remove(index_lock)
      except OSError as exc:
        if exc.errno != errno.ENOENT:
          logging.warn('failed to remove %r, reset will fail: %s', index_lock,
                       exc)
      _git_check_call(['reset', '-q', '--hard', revision], cwd=engine_path)

    # If the engine has refactored/moved modules we need to clean all .pyc files
    # or things will get squirrely.
    _git_check_call(['clean', '-qxf'], cwd=engine_path)

  return engine_path, py3_only


class RequiredBinaryNotFound(Exception):
  pass


def main() -> int:
  try:
    for required_binary in REQUIRED_BINARIES:
      if not _is_on_path(required_binary):
        raise RequiredBinaryNotFound(
            f'Required binary is not found on PATH: {required_binary}')

    if '--verbose' in sys.argv:
      logging.getLogger().setLevel(logging.INFO)

    args = sys.argv[1:]
    engine_override, recipes_cfg_path = parse_args(args)

    if recipes_cfg_path:
      # calculate repo_root from recipes_cfg_path
      repo_root = os.path.dirname(
          os.path.dirname(os.path.dirname(recipes_cfg_path)))
    else:
      # find repo_root with git and calculate recipes_cfg_path
      repo_root = (
          _git_output(['rev-parse', '--show-toplevel'],
                      cwd=os.path.abspath(os.path.dirname(__file__))).decode())
      recipes_cfg_path = os.path.join(repo_root, 'infra', 'config',
                                      'recipes.cfg')
      args = ['--package', recipes_cfg_path] + args
    engine_path, py3_only = checkout_engine(engine_override, repo_root,
                                            recipes_cfg_path)

    using_py3 = py3_only or os.getenv('RECIPES_USE_PY3') == 'true'
    vpython = ('vpython' + ('3' if using_py3 else '') + _BAT)
    if not _is_on_path(vpython):
      raise RequiredBinaryNotFound(
          f'Required binary is not found on PATH: {vpython}')

    argv = ([
        vpython,
        '-u',
        os.path.join(engine_path, 'recipe_engine', 'main.py'),
    ] + args)

    if IS_WIN:
      # No real 'exec' on windows; set these signals to ignore so that they
      # propagate to our children but we still wait for the child process to quit.
      import signal
      signal.signal(signal.SIGBREAK,
                    signal.SIG_IGN)  # type: ignore # SIGBREAK is windows-only
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      return _subprocess_call(argv)
    else:
      os.execvp(argv[0], argv)
  except RequiredBinaryNotFound as e:
    print(str(e))
    return 1


if __name__ == '__main__':
  sys.exit(main())
