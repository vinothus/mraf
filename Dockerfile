FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV IDF_PATH=/opt/esp/idf

# -----------------------------------------------------------------------------
# Base packages
# -----------------------------------------------------------------------------
RUN apt-get update && apt-get install -y \
    curl \
    wget \
    unzip \
    git \
    ca-certificates \
    gnupg \
    lsb-release \
    software-properties-common \
    build-essential \
    cmake \
    ninja-build \
    ccache \
    gperf \
    dfu-util \
    device-tree-compiler \
    libffi-dev \
    libssl-dev \
    libusb-1.0-0 \
    python3 \
    python3-pip \
    python3-dev \
    python3-venv \
    python3-setuptools \
    python3-wheel \
    pyserial \
    nano \
    vim \
    jq \
    zip \
    xz-utils \
    file \
    make \
    gcc \
    g++ \
    sudo \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
# Install Node.js 22 LTS
# -----------------------------------------------------------------------------
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - && \
    apt-get update && \
    apt-get install -y nodejs && \
    rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
# Install OpenCode
# -----------------------------------------------------------------------------
RUN npm install -g opencode-ai

# -----------------------------------------------------------------------------
# Install West and Zephyr Python dependencies
# -----------------------------------------------------------------------------
RUN python3 -m pip install --break-system-packages --upgrade pip && \
    python3 -m pip install --break-system-packages \
    west \
    pyelftools \
    pyserial \
    pyyaml \
    cryptography \
    intelhex \
    packaging \
    pytest \
    six

# -----------------------------------------------------------------------------
# Create non-root user
# -----------------------------------------------------------------------------
RUN useradd -m -s /bin/bash opencode && \
    usermod -aG sudo opencode && \
    echo "opencode ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

# -----------------------------------------------------------------------------
# Git configuration
# -----------------------------------------------------------------------------
RUN git config --system --add safe.directory '*'

# -----------------------------------------------------------------------------
# Install ESP-IDF v5.5
# -----------------------------------------------------------------------------
RUN mkdir -p /opt/esp && \
    git clone --recursive -b v5.5 https://github.com/espressif/esp-idf.git /opt/esp/idf

RUN /opt/esp/idf/install.sh esp32

# -----------------------------------------------------------------------------
# Automatically load ESP-IDF environment
# -----------------------------------------------------------------------------
RUN echo 'source /opt/esp/idf/export.sh >/dev/null 2>&1' >> /etc/bash.bashrc

# -----------------------------------------------------------------------------
# Verify installation
# -----------------------------------------------------------------------------
RUN node --version && \
    npm --version && \
    python3 --version && \
    west --version && \
    ninja --version

RUN bash -c "source /opt/esp/idf/export.sh && idf.py --version"

# -----------------------------------------------------------------------------
# Copy project
# -----------------------------------------------------------------------------
COPY . /workspace

WORKDIR /workspace

# -----------------------------------------------------------------------------
# Run as non-root user
# -----------------------------------------------------------------------------
USER opencode

CMD ["/bin/bash"]
