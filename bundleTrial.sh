#! /bin/sh

cd /Users/paulj/Projects/GeanyBundle
export PATH=$PREFIX/bin:~/.local/bin:$PATH
~/.local/bin/jhbuild run gtk-mac-bundler AppGameKitTrial.bundle
