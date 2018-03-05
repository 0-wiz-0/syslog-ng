FROM ubuntu:17.10
MAINTAINER Andras Mitzki <andras.mitzki@balabit.com>, Laszlo Szemere <laszlo.szemere@balabit.com>

ADD helpers/functions.sh .

# Install packages
RUN apt-get update -qq && apt-get install --no-install-recommends -y \
    python-pip \
    python-setuptools \
    wget

ADD required-packages/artful-pip.txt .
RUN cat artful-pip.txt | grep -v "#" | xargs pip install 

ADD required-packages/artful-dist.txt .
RUN cat artful-dist.txt | grep -v "#" | xargs apt-get install --no-install-recommends -y

ADD required-packages/artful-obs.txt .
RUN ./functions.sh add_obs_repo_debian xUbuntu_17.04
RUN cat artful-obs.txt | grep -v "#" | xargs apt-get install --no-install-recommends -y


RUN cd /tmp && wget http://ftp.de.debian.org/debian/pool/main/libn/libnative-platform-java/libnative-platform-jni_0.11-5_$(dpkg --print-architecture).deb
RUN cd /tmp && dpkg -i libnative-platform-jni*.deb

# grab gosu for easy step-down from root
ADD helpers/gosu.pubkey /tmp
RUN ./functions.sh step_down_from_root_with_gosu $(dpkg --print-architecture)


# mount points for source code
RUN mkdir /source
VOLUME /source
VOLUME /build


ADD helpers/entrypoint-debian.sh /
ENTRYPOINT ["/entrypoint-debian.sh"]
