# syntax=docker/dockerfile:1

FROM debian:bullseye as devenv

RUN echo "deb http://deb.debian.org/debian bullseye-backports main" >> /etc/apt/sources.list

RUN apt update
RUN apt install swig -y
RUN apt install libjansson4 libjansson-dev nlohmann-json3-dev -y
RUN apt install doxygen graphviz aspell -y
RUN apt install git -y
RUN apt install python3 python3-dev -y
RUN apt install redis-server -y
RUN apt install libhiredis0.14 -y
RUN apt install libhiredis-dev -y
RUN apt install libzmq5 libzmq3-dev libzmq5-dev -y
RUN apt install libtool -y
RUN apt install autoconf automake autoconf-archive -y
RUN apt install dh-exec -y
RUN apt install libgtest-dev -y
RUN apt install libgmock-dev -y

RUN apt install pip -y
RUN mkdir /var/debs
COPY *.deb /var/debs/

WORKDIR /var/debs
RUN rm libyang2*
RUN rm frr*
RUN rm libpython3.9_3.9.2-1+fips_amd64.deb libpython3.9-minimal_3.9.2-1+fips_amd64.deb libpython3.9-stdlib_3.9.2-1+fips_amd64.deb python3.9_3.9.2-1+fips_amd64.deb python3.9-minimal_3.9.2-1+fips_amd64.deb
RUN dpkg -i /var/debs/* ; apt install -f -y || true
RUN pip install compiledb
RUN chmod -R 777 /root

WORKDIR /var/sonic-buildimage
CMD ["bash"]
