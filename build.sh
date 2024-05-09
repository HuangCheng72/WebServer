#!/bin/bash

echo "开始编译项目..."
make

if [ $? -eq 0 ]; then
    echo "编译成功!"
else
    echo "编译失败!"
fi

