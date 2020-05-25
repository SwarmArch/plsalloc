import os
Import('swarmIncludePath')

# TODO(mcj) should this import a stripped-down environment from the SConstruct?
env = Environment(ENV = os.environ)
if GetOption('clang'):
    env['CC'] = 'clang'
    env['CXX'] = 'clang++'

env.Append(CPPPATH = [swarmIncludePath])
env.Append(CPPFLAGS = ['-std=c++14', '-Wall', '-Werror', '-O3', '-gdwarf-3',])
env.Append(CPPDEFINES = ['NASSERT', 'NDEBUG'])

libplsalloc = env.StaticLibrary(target='plsalloc', source=['plsalloc.cpp'])

Return('libplsalloc')
