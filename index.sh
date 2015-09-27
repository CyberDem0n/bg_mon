#!/bin/bash

echo -e "#include <event2/buffer.h>" > index.c
echo -e "#include \"index.h\"\n" >> index.c
echo -e "void index_html(struct evbuffer *evb)\n{" >> index.c
echo -en "\tevbuffer_add_printf(evb, \"" >> index.c
sed 's/$/\\n\\/g' index.html >> index.c
echo -e "\");\n}" >> index.c
