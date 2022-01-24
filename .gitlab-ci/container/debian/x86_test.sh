#!/bin/sh

set -ex

#
# Copy platform dependent <TEST_TYPE>-*.txt files and remove test type prefix.
# Additionally, deploy *.toml and *.yml files.
#
# arg1: test platform (host, virt)
#
deploy_expectations() {
    local tp=${1:-host} tt file
    [ -z "${DEQP_SUITE}" ] && tt=piglit || tt=deqp

    for file in ${CI_PROJECT_DIR}/.gitlab-ci/expectations/${tp}/${tt}-*.txt; do
        cp -a ${file} install/$(printf "%s" "${file##*/}" | sed "s/^${tt}-//")
    done

    cp -a ${CI_PROJECT_DIR}/.gitlab-ci/expectations/${tp}/*.toml \
        ${CI_PROJECT_DIR}/.gitlab-ci/expectations/${tp}/*.yml \
        install/
}

if [ ${MESA_PROJECT_PATH} = "mesa/mesa" ]; then
    MESA_CI_PROJECT_DIR="/builds/${MESA_PROJECT_PATH}"
else
    # Hack to allow testing with a Mesa CI fork
    MESA_CI_PROJECT_DIR="/builds/mesa/mesa"

    # Sometimes LD_LIBRARY_PATH seem to be ignored by dEQP MESA-LOADER since
    # it still tries to load drivers from paths specific to Mesa container.
    export LIBGL_DRIVERS_PATH=${MESA_CI_PROJECT_DIR}/install/lib/dri
fi

mkdir -p ${MESA_CI_PROJECT_DIR}
cd ${MESA_CI_PROJECT_DIR}

# Deploy Mesa CI artifacts
wget -S --progress=dot:giga -O- \
    https://${MINIO_HOST}/artifacts/${MESA_PROJECT_PATH}/${MESA_PIPELINE_ID}/mesa-amd64.tar.gz \
| tar -xvz

# Overwrite Mesa CI's virglrenderer binaries with self built versions
cp -a ${CI_PROJECT_DIR}/install/bin/virgl_test_server /usr/local/bin/
cp -a ${CI_PROJECT_DIR}/install/lib/libvirglrenderer.so* /usr/local/lib/

if [ "${GALLIUM_DRIVER}" = "virgl" ]; then
    #
    # Run the tests on virtual platform (virgl/crosvm)
    #
    deploy_expectations virt

    #
    # crosvm-runner.sh depends on resources from ${CI_PROJECT_DIR}/install,
    # but their actual location is ${MESA_CI_PROJECT_DIR}/install, hence
    # let's fix this using a bind mount.
    #
    mv ${CI_PROJECT_DIR}/install ${CI_PROJECT_DIR}/install-orig
    mkdir ${CI_PROJECT_DIR}/install
    mount --bind install ${CI_PROJECT_DIR}/install

    set +e

    # Use all threads for rendering and only run one job at a time
    LP_NUM_THREADS=${FDO_CI_CONCURRENT} \
    FDO_CI_CONCURRENT=1 \
    LD_LIBRARY_PATH=${CI_PROJECT_DIR}/install/lib \
        install/crosvm-runner.sh "${TEST_SCRIPT}"

    RET=$?

    # Cleanup
    umount ${CI_PROJECT_DIR}/install && \
    rmdir ${CI_PROJECT_DIR}/install && \
    mv ${CI_PROJECT_DIR}/install-orig ${CI_PROJECT_DIR}/install
else
    #
    # Run the tests on host platform (virpipe/vtest)
    #
    deploy_expectations host

    export LIBGL_ALWAYS_SOFTWARE="true"
    set +e

    if [ -z "${DEQP_SUITE}" ]; then
        PIGLIT_RUNNER_OPTIONS="--timeout 180" \
            install/piglit/piglit-runner.sh
    else
        DEQP_EXPECTED_RENDERER=virgl \
        WAFFLE_PLATFORM="surfaceless_egl" \
        SANITY_MESA_VERSION_CMD=wflinfo \
        HANG_DETECTION_CMD= \
        EGL_PLATFORM=surfaceless \
            install/deqp-runner.sh
    fi

    RET=$?
fi

mv -f results ${CI_PROJECT_DIR}/
exit ${RET}
