import multiprocessing
import os
import shutil
import subprocess

from build_scripts.component_configurator import ConfiguratorStub
from build_scripts.jom_settings import JomSettings
from build_scripts.openssl_settings import OpenSslSettings


class QtSettingsStub(ConfiguratorStub):
    def __init__(self, settings, package_name=None, package_path=None, display_name=None):
        ConfiguratorStub.__init__(self, settings)
        self.jom = JomSettings(settings)
        self.openssl = OpenSslSettings(settings)

        self._release = '5.12'
        self._version = self._release + '.2'
        self._package_path = package_path if package_path else 'submodules'
        self._package_name = package_name + '-' + self._version
        self._script_revision = '8'

        if self._project_settings.on_windows():
            self._package_ext = 'zip'
        else:
            self._package_ext = 'tar.xz'

        self._package_url = 'https://download.qt.io/official_releases/qt/{}/{}/{}/{}.{}'.format(
            self._release,
            self._version,
            self._package_path,
            self._package_name,
            self._package_ext,
        )

        self._install_name = 'Qt5'
        self._display_name = display_name if display_name else 'Qt5'

    def config(self, command=None):
        if command is None:
            command = []

        modules_to_skip = ['doc', 'webchannel', 'webview', 'sensors', 'serialport',
                           'script', 'multimedia', 'wayland', 'location', 'webglplugin', 'gamepad',
                           'purchasing', 'canvas3d', 'speech', '3d', 'androidextras', 'canvas3d',
                           'connectivity', 'virtualkeyboard']
        if self._project_settings.get_link_mode() == 'static':
            modules_to_skip.append('imageformats')

        sql_drivers_to_skip = ['db2', 'oci', 'tds', 'sqlite2', 'odbc', 'ibase', 'psql']

        if self._project_settings.on_windows():
            command.insert(0, os.path.join(self.get_unpacked_sources_dir(), 'configure.bat'))
            command.append('-platform')
            command.append('win32-msvc' + self._project_settings.get_vs_year())
        else:
            command.insert(0, os.path.join(self.get_unpacked_sources_dir(), 'configure'))

        if self._project_settings.get_build_mode() == 'release':
            command.append('-release')
        else:
            command.append('-debug')

        if self._project_settings.on_linux():
            command.append('-dbus')
        else:
            command.append('-no-dbus')

        if self._project_settings.get_link_mode() == 'static':
            command.append('-static')
            command.append('-openssl-linked')
            if self._project_settings.on_windows():
                command.append('-static-runtime')

        command.append('-confirm-license')
        command.append('-opensource')
        command.append('-no-opengl')
        command.append('-qt-pcre')
        command.append('-qt-harfbuzz')
        command.append('-sql-sqlite')
        command.append('-sql-mysql')
        command.append('-no-feature-vulkan')

        command.append('-I{}'.format(os.path.join(self.openssl.get_install_dir(),'include')))

        if self._project_settings.on_osx():
            command.append('-L/usr/local/opt/mysql@5.7/lib')
            command.append('-I/usr/local/opt/mysql@5.7/include')
            command.append('-I/usr/local/opt/mysql@5.7/include/mysql')

        if self._project_settings.on_linux():
            command.append('-system-freetype')
            command.append('-fontconfig')

            command.append('-no-glib')
            command.append('-cups')
            command.append('-no-icu')
            command.append('-nomake')
            command.append('tools')
        else:
            command.append('-qt-libpng')
            command.append('-no-freetype')

        if self._project_settings.on_windows():
            command.append('-IC:\Program Files\MySQL\MySQL Connector C 6.1\include')
            command.append('-LC:\Program Files\MySQL\MySQL Connector C 6.1\lib')

        command.append('-nomake')
        command.append('tests')
        command.append('-nomake')
        command.append('examples')

        for driver in sql_drivers_to_skip:
            command.append('-no-sql-' + driver)

        for module in modules_to_skip:
            command.append('-skip')
            command.append(module)

        command.append('-prefix')
        command.append(self.get_install_dir())

        ssldir_var = self.openssl.get_install_dir()
        ssllibs_var = '-L{} -lssl -lcrypto'.format(os.path.join(self.openssl.get_install_dir(),'lib'))
        sslinc_var = os.path.join(self.openssl.get_install_dir(),'include')

        if self._project_settings.on_linux():
            ssllibs_var += ' -ldl -lpthread'
        elif self._project_settings.on_windows():
            ssllibs_var += ' -lUser32 -lAdvapi32 -lGdi32 -lCrypt32 -lws2_32'

        compile_variables = os.environ.copy()
        compile_variables['OPENSSL_DIR'] = ssldir_var
        compile_variables['OPENSSL_LIBS'] = ssllibs_var
        compile_variables['OPENSSL_INCLUDE'] = sslinc_var

        result = subprocess.call(command, env=compile_variables)
        if result != 0:
            print('Configure of {} failed'.format(self._display_name))
            return False

        return True

    def make(self):
        command = []

        if self._project_settings.on_windows():
            command.append(self.jom.get_executable_path())
            if self._project_settings.get_link_mode() == 'static':
                command.append('mode=static')
        else:
            command.append('make')
            command.append('-j')
            command.append(str(max(1, multiprocessing.cpu_count() - 1)))

        result = subprocess.call(command)
        if result != 0:
            print('{} make failed'.format(self._display_name))
            return False

        return True

    def install(self):
        command = []
        if self._project_settings.on_windows():
            command.append('nmake')
        else:
            command.append('make')

        command.append('install')
        result = subprocess.call(command)
        if result != 0:
            print('{} install failed'.format(self._display_name))
            return False

        return True


class QtSettings(QtSettingsStub):
    def __init__(self, settings):
        QtSettingsStub.__init__(self, settings, package_name='qt-everywhere-src', package_path='single', display_name='Qt5')

    def config(self):
        QtSettingsStub.config(self, ['-no-qml-debug'])
        return True


class QtBaseSettings(QtSettingsStub):
    def __init__(self, settings):
        QtSettingsStub.__init__(self, settings, package_name='qtbase-everywhere-src', display_name='Qt5Base')


class QtSubModuleSettings(QtSettingsStub):
    def __init__(self, settings, package_name, display_name):
        QtSettingsStub.__init__(self, settings, package_name=package_name, display_name=display_name)
        self.qmake = os.path.join(QtBaseSettings(settings).get_install_dir(), 'bin', 'qmake')

    def GetRevisionFileName(self):
        return os.path.join(self.get_install_dir(), '3rd_revision.{}.txt'.format(self._display_name))

    def clean_install_dir(self):
        pass

    def config(self):
        os.chdir(self.get_unpacked_sources_dir())

        command = []
        command.append(self.qmake)
        command.append('-o')
        command.append(os.path.join(self.get_build_dir(), 'Makefile'))

        result = subprocess.call(command)

        os.chdir(self.get_build_dir())

        if result != 0:
            print('{} configure failed'.format(self._install_name))
            return False

        return True


class QtDeclarativeSettings(QtSubModuleSettings):
    def __init__(self, settings):
        QtSubModuleSettings.__init__(self, settings, package_name='qtdeclarative-everywhere-src', display_name='Qt5Qml')


class QtQuickControlsSettings(QtSubModuleSettings):
    def __init__(self, settings):
        QtSubModuleSettings.__init__(self, settings, package_name='qtquickcontrols-everywhere-src', display_name='Qt5QuickControls')


class QtQuickControls2Settings(QtSubModuleSettings):
    def __init__(self, settings):
        QtSubModuleSettings.__init__(self, settings, package_name='qtquickcontrols2-everywhere-src', display_name='Qt5QuickControls2')


class QtChartsSettings(QtSubModuleSettings):
    def __init__(self, settings):
        QtSubModuleSettings.__init__(self, settings, package_name='qtcharts-everywhere-src', display_name='Qt5Charts')


class QtSvgSettings(QtSubModuleSettings):
    def __init__(self, settings):
        QtSubModuleSettings.__init__(self, settings, package_name='qtsvg-everywhere-src', display_name='Qt5Svg')
