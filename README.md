Twinkle Lisp
============

Twinkle Lisp is a simple language that can be used to implement an embeddable application server.
It's currently used in Twinkle Notes.  

Its syntax is largely based on Scheme,
but we have let it grow wild to suit our own taste,
and thus we can not call it Scheme any more.

Its process model is like Erlang, each process running in its own tiny virtual machine. Processes cooperate by sending messages to each other.

For remote server connection, we build a customized secure socket connection, because we want it to be simple and understandable,
and not to rely on certificates from authorities to make remote processes talk securely.

External dependency
===================

- OpenSSL 1.1+ (only libcrypto are used)
- SQLite3, or SQLCipher(SQLite3 with encryption). You can customize in Makefile.

How to build
============

This project is based on c99, and by default we use GNU Make to build the executable.

On windows, you have to use mingw-win32, and only windows 7+ is officially supported.
We assume that you install mingw and dependency libs in certain locations, please see Makefile for detail.

License
=======

Unless specified individually or those originated from other projects,
files from this project are released under AGPL license (See COPYING).
