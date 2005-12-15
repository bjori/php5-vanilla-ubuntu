dnl
dnl $Id: config.m4,v 1.3 2005/05/29 23:16:45 sniper Exp $
dnl

PHP_ARG_WITH(xmlreader, for XMLReader support,
[  --with-xmlreader        Include XMLReader support])

if test -z "$PHP_LIBXML_DIR"; then
  PHP_ARG_WITH(libxml-dir, libxml2 install dir,
  [  --with-libxml-dir=DIR     XMLReader: libxml2 install prefix], no, no)
fi

if test "$PHP_XMLREADER" != "no"; then

  if test "$PHP_LIBXML" = "no"; then
    AC_MSG_ERROR([XMLReader extension requires LIBXML extension, add --enable-libxml])
  fi

  PHP_SETUP_LIBXML(XMLREADER_SHARED_LIBADD, [
    AC_DEFINE(HAVE_XMLREADER,1,[ ])
    PHP_NEW_EXTENSION(xmlreader, php_xmlreader.c, $ext_shared)
    PHP_SUBST(XMLREADER_SHARED_LIBADD)
  ], [
    AC_MSG_ERROR([xml2-config not found. Please check your libxml2 installation.])
  ])
fi