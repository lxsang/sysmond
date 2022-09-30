def do_build()
{
  sh '''
  set -e
  cd $WORKSPACE
  mkdir -p build/$arch/etc/systemd/system/
  [ -f Makefile ] && make clean
  libtoolize
  aclocal
  autoconf
  automake --add-missing
  ./configure --prefix=/usr
  make
  DESTDIR=$WORKSPACE/build/$arch make install
  '''
}

pipeline{
  agent { node{ label'master' }}
  options {
    // Limit build history with buildDiscarder option:
    // daysToKeepStr: history is only kept up to this many days.
    // numToKeepStr: only this many build logs are kept.
    // artifactDaysToKeepStr: artifacts are only kept up to this many days.
    // artifactNumToKeepStr: only this many builds have their artifacts kept.
    buildDiscarder(logRotator(numToKeepStr: "1"))
    // Enable timestamps in build log console
    timestamps()
    // Maximum time to run the whole pipeline before canceling it
    timeout(time: 1, unit: 'HOURS')
    // Use Jenkins ANSI Color Plugin for log console
    ansiColor('xterm')
    // Limit build concurrency to 1 per branch
    disableConcurrentBuilds()
  }
  stages
  {
    stage('Build AMD64') {
      agent {
          docker {
              image 'xsangle/ci-tools:bionic-amd64'
              reuseNode true
          }
      }
      steps {
        script{
          env.arch = "amd64"
        }
        do_build()
      }
    }
    stage('Build ARM64') {
      agent {
          docker {
              image 'xsangle/ci-tools:bionic-arm64'
              reuseNode true
          }
      }
      steps {
        script{
          env.arch = "arm64"
        }
        do_build()
      }
    }
    stage('Build ARM') {
      agent {
          docker {
              image 'xsangle/ci-tools:bionic-arm'
              reuseNode true
          }
      }
      steps {
        script{
          env.arch = "arm"
        }
        do_build()
      }
    }
    stage("Archive") {
      steps{
        script {
            archiveArtifacts artifacts: 'build/', fingerprint: true, onlyIfSuccessful: true
        }
      }
    }
  }
}
