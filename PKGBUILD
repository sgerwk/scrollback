pkgname=scrollback
pkgver=1.0.0
pkgrel=1
pkgdesc="scrollback buffer for Linux virtual terminals"
arch=('x86_64')
url="https://github.com/sgerwk/scrollback"
depends=()
source=(git+https://github.com/sgerwk/scrollback)
sha256sums=('SKIP')

build() {
  cd $srcdir/$pkgname
  make || return 1
}

check() {
  cd $srcdir/$pkgname
  return $(test -x scrollback)
}

package() {
  cd $srcdir/$pkgname
  make DESTDIR=$pkgdir install
}

