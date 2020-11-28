Scrollback
==========

A scrollback buffer for the linux console

How to run it
-------------

Calling ``scrollback /bin/bash`` in a linux console makes the shell run with a
scrollback buffer.

It can be called from ``.bashrc`` as well:

```
! $SCROLLBACK false && scrollback /bin/bash
```

Once checked that it works this way, it can be run to replace the current
shell:

```
! $SCROLLBACK false && scrollback -c /bin/bash && exec scrollback /bin/bash
```

Keys
----

The keys for scrolling are F11 and F12. The old combinations shift-pageup and
shift-pagedown can be used instead by calling ``scrollback -k`` at least once
as root. This is done by systemctl with the included ``scrollback.service``
file.

