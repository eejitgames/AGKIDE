#! /bin/sh

cd /Users/paulj/Documents/SVN/AGKTrunk/IDE/Geany-1.24.1
/usr/bin/make install
cp ../../CompilerNew/build/Release/AGKCompiler /Users/paulj/Projects/Geany-Compiled/share/applications/AGKCompiler
cp ../../CompilerNew/build/Release/CommandList.dat /Users/paulj/Projects/Geany-Compiled/share/applications/CommandList.dat
mkdir /Users/paulj/Projects/Geany-Compiled/share/applications/interpreters
cp -R ../../apps/interpreter_mac/build/Release/AGK\ Player.app /Users/paulj/Projects/Geany-Compiled/share/applications/interpreters/Mac.app
cp -R /Volumes/Shared/Help /Users/paulj/Projects/Geany-Compiled/share/Help
cd /Users/paulj/Projects/GeanyBundle
export PATH=$PREFIX/bin:~/.local/bin:$PATH
~/.local/bin/jhbuild run gtk-mac-bundler AGK2.bundle
cd /Users/paulj/Projects
rm -rf Geany-Compiled