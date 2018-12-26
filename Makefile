CXX = g++
CPPFLAGS = -g -Wall -std=c++14 -fsanitize=address -fno-omit-frame-pointer
EXEC = jit
SOURCES = $(shell ls ./*.cc | xargs -n 1 basename)
OBJECTS = ${SOURCES:.cc=.o}
DEPENDS = ${OBJECTS:.o=.d}

${EXEC}: ${OBJECTS}
	${CXX} ${CXXFLAGS} ${OBJECTS} -o ${EXEC} -lasan

-include ${DEPENDS}

.PHONY: clean
clean:
	rm -f *.o *.d $(EXEC)
