/*
* Copyright (c) YugaByte, Inc.
 */

package common

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/viper"
	"github.com/vmware-labs/yaml-jsonpath/pkg/yamlpath"
	"gopkg.in/yaml.v3"

	log "github.com/yugabyte/yugabyte-db/managed/yba-installer/logging"
	// "github.com/yugabyte/yugabyte-db/managed/yba-installer/preflight"
)

// Hardcoded Variables.

// Systemctl linux command.
const Systemctl string = "systemctl"

// InputFile where installer config settings are specified.
var InputFile = "/opt/yba-ctl/yba-ctl.yml"

var YbaCtlLogFile = "/opt/yba-ctl/yba-ctl.log"

var installingFile = "/opt/yba-ctl/.installing"

// InstalledFile is location of install completed marker file.
var InstalledFile = "/opt/yba-ctl/.installed"

var PostgresPackageGlob = "postgresql-*-linux-x64-binaries.tar.gz"

var skipConfirmation = false

var yumList = []string{"RedHat", "CentOS", "Oracle", "Alma", "Amazon"}

var aptList = []string{"Ubuntu", "Debian"}

var goBinaryName = "yba-ctl"

var versionMetadataJSON = "version_metadata.json"

var javaBinaryGlob = "OpenJDK8U-jdk_x64_linux_*.tar.gz"

var pemToKeystoreConverter = "pemtokeystore-linux-amd64"

// DetectOS detects the operating system yba-installer is running on.
func DetectOS() string {

	command1 := "bash"
	args1 := []string{"-c", "awk -F= '/^NAME/{print $2}' /etc/os-release"}
	output, _ := ExecuteBashCommand(command1, args1)

	return string(output)
}

// GetVersion gets the version at execution time so that yba-installer
// installs the correct version of Yugabyte Anywhere.
func GetVersion() string {

	// locate the version metadata json file in the same dir as the yba-ctl
	// binary
	var configViper = viper.New()
	configViper.SetConfigName(versionMetadataJSON)
	configViper.SetConfigType("json")
	configViper.AddConfigPath(GetBinaryDir())

	err := configViper.ReadInConfig()
	if err != nil {
		panic(err)
	}

	versionNumber := fmt.Sprint(configViper.Get("version_number"))
	buildNumber := fmt.Sprint(configViper.Get("build_number"))
	if os.Getenv("YBA_MODE") == "dev" {
		// in dev itest builds, build_number is set to PRE_RELEASE
		buildNumber = fmt.Sprint(configViper.Get("build_id"))
	}

	version := versionNumber + "-b" + buildNumber

	if !IsValidVersion(version) {
		log.Fatal(fmt.Sprintf("Invalid version in metadata file '%s'", version))
	}

	return version
}

// ExecuteBashCommand exeuctes a command in the shell, returning the output and error.
func ExecuteBashCommand(command string, args []string) (o string, e error) {

	fullCmd := command + " " + strings.Join(args, " ")
	startTime := time.Now()
	log.Debug("About to run command " + fullCmd)
	cmd := exec.Command(command, args...)

	var execOut bytes.Buffer
	var execErr bytes.Buffer
	cmd.Stdout = &execOut
	cmd.Stderr = &execErr

	err := cmd.Run()

	if err == nil {
		log.Debug(fmt.Sprintf("Completed running command: %s [took %f secs]", fullCmd, time.Since(startTime).Seconds()))
		log.Trace(fmt.Sprintf("Stdout for command %s was \n%s\n", fullCmd, execOut.String()))
		log.Trace(fmt.Sprintf("Stderr for command %s was \n%s\n", fullCmd, execErr.String()))
	} else {
		err = fmt.Errorf("command failed with error %w and stderr %s", err, execErr.String())
		log.Info("ERROR: '" + fullCmd + "' failed with error " +
			err.Error() + "\nPrinting stdOut/stdErr " + execOut.String() + execErr.String())
	}

	return execOut.String(), err
}

// IndexOf returns the index in arr where val is present, -1 otherwise.
func IndexOf(arr []string, val string) int {

	for pos, v := range arr {
		if v == val {
			return pos
		}
	}

	return -1
}

// Contains checks an array values for the presence of target.
// Type must be a comparable
func Contains[T comparable](values []T, target T) bool {
	for _, v := range values {
		if v == target {
			return true
		}
	}
	return false
}

// Chown changes ownership of dir to user:group, recursively (optional).
func Chown(dir, user, group string, recursive bool) error {
	args := []string{fmt.Sprintf("%s:%s", user, group), dir}
	if recursive {
		args = append([]string{"-R"}, args...)
	}
	_, err := ExecuteBashCommand("chown", args)
	return err
}

// HasSudoAccess determines whether or not running user has sudo permissions.
func HasSudoAccess() bool {

	cmd := exec.Command("id", "-u")
	output, err := cmd.Output()
	if err != nil {
		log.Fatal("Error: " + err.Error() + ".")
	}

	i, err := strconv.Atoi(string(output[:len(output)-1]))
	if err != nil {
		log.Fatal("Error: " + err.Error() + ".")
	}

	if i == 0 {
		return true
	}
	return false
}

// GetCurrentUser returns the user yba-ctl was run as.
func GetCurrentUser() string {
	user, err := user.Current()
	if err != nil {
		log.Fatal(fmt.Sprintf("Error %s getting current user", err.Error()))
	}
	return user.Username
}

// CopyFileGolang copies src file to dst.
// Assumes both src/dst are valid absolute paths and dst file parent directory is already created.
func CopyFileGolang(src string, dst string) {

	bytesRead, errSrc := os.ReadFile(src)

	if errSrc != nil {
		log.Fatal("Error: " + errSrc.Error() + ".")
	}
	errDst := os.WriteFile(dst, bytesRead, 0644)
	if errDst != nil {
		log.Fatal("Error: " + errDst.Error() + ".")
	}

	log.Debug("Copy from " + src + " to " + dst + " executed successfully.")
}

// CreateDir creates a directory according to the given permissions, logging an error if necessary.
func CreateDir(dir string, perm os.FileMode) {
	err := os.MkdirAll(dir, perm)
	if err != nil && !os.IsExist(err) {
		log.Fatal(fmt.Sprintf("Error creating %s. Failed with %s", dir, err.Error()))
	}
}

// MoveFileGolang moves (renames) a src file to dst.
func MoveFileGolang(src string, dst string) {

	err := os.Rename(src, dst)
	if err != nil {
		log.Fatal("Error: " + err.Error() + ".")
	}

	log.Debug("Move from " + src + " to " + dst + " executed successfully.")

}

// Create a file at a relative path for the non-root case. Have to make the directory before
// inserting the file in that directory.
func Create(p string) (*os.File, error) {
	if err := os.MkdirAll(filepath.Dir(p), 0777); err != nil {
		log.Fatal(fmt.Sprintf("Error creating %s. Failed with %s", p, err.Error()))
		return nil, err
	}
	return os.Create(p)
}

// CreateSymlink of binary from pkgDir to linkDir.
/*
	pkgDir - directory where the binary (file or directory) is located
	linkDir - directory where you want the link to be created
	binary - name of file or directory to link. should already exist in pkgDir and will be the same
*/
func CreateSymlink(pkgDir string, linkDir string, binary string) {
	binaryPath := fmt.Sprintf("%s/%s", pkgDir, binary)
	linkPath := fmt.Sprintf("%s/%s", linkDir, binary)

	args := []string{"-sf", binaryPath, linkPath}
	ExecuteBashCommand("ln", args)
}

type defaultAnswer int

func (d defaultAnswer) String() string {
	return strconv.Itoa(int(d))
}

const (
	DefaultNone defaultAnswer = iota
	DefaultYes
	DefaultNo
)

// DisableUserConfirm skips all confirmation steps.
func DisableUserConfirm() {
	skipConfirmation = true
}

// UserConfirm asks the user for confirmation before proceeding.
// Returns true if the user is ok with proceeding (or if --force is spec'd)
func UserConfirm(prompt string, defAns defaultAnswer) bool {
	if skipConfirmation {
		return true
	}
	if !strings.HasSuffix(prompt, " ") {
		prompt = prompt + " "
	}
	var selector string
	switch defAns {
	case DefaultNone:
		selector = "[yes/no]"
	case DefaultYes:
		selector = "[YES/no]"
	case DefaultNo:
		selector = "[yes/NO]"
	default:
		log.Fatal("unknown defaultanswer " + defAns.String())
	}

	for {
		fmt.Printf("%s%s: ", prompt, selector)
		reader := bufio.NewReader(os.Stdin)
		input, err := reader.ReadString('\n')
		if err != nil {
			fmt.Println("invalid input: " + err.Error())
			continue
		}
		input = strings.TrimSuffix(input, "\n")
		input = strings.Trim(input, " ")
		input = strings.ToLower(input)
		switch input {
		case "n", "no":
			return false
		case "y", "yes":
			return true
		case "":
			if defAns == DefaultYes {
				return true
			} else if defAns == DefaultNo {
				return false
			}
			fallthrough
		default:
			fmt.Println("please enter 'yes' or 'no'")
		}
	}
}

func InitViper() {
	// Init Viper
	viper.SetDefault("service_username", DefaultServiceUser)
	viper.SetDefault("installRoot", "/opt/yugabyte")
	viper.SetConfigFile(InputFile)
	viper.ReadInConfig()
}

func GetBinaryDir() string {

	exPath, err := os.Executable()
	if err != nil {
		log.Fatal("Error determining yba-ctl binary path.")
	}
	realPath, err := filepath.EvalSymlinks(exPath)
	if err != nil {
		log.Fatal("Error eval symlinks for yba-ctl binary path.")
	}
	return filepath.Dir(realPath)
}

func GetReferenceYaml() string {
	return filepath.Join(GetBinaryDir(), "yba-ctl.yml.reference")
}

type YBVersion struct {

	// ex: 2.17.1.0-b235-foo
	// major, minor, patch, subpatch in order
	// ex: 2.17.1.0
	PublicVersionDigits []int

	// ex: 235
	BuildNum int

	// ex: foo
	Remainder string
}

func NewYBVersion(versionString string) (*YBVersion, error) {
	version := &YBVersion{
		PublicVersionDigits: []int{-1, -1, -1, -1},
		BuildNum:            -1,
	}

	if versionString == "" {
		return version, nil
	}

	re := regexp.MustCompile(
		`^(?P<major>[0-9]+)\.(?P<minor>[0-9]+)\.(?P<patch>[0-9]+)\.(?P<subpatch>[0-9]+)(?:-b(?P<build>[0-9]+)[-a-z]*)?$`)
	matches := re.FindStringSubmatch(versionString)
	if matches == nil || len(matches) < 5 {
		return version, fmt.Errorf("invalid version string %s", versionString)
	}

	var err error
	version.PublicVersionDigits[0], err = strconv.Atoi(matches[1])
	if err != nil {
		return version, err
	}

	version.PublicVersionDigits[1], err = strconv.Atoi(matches[2])
	if err != nil {
		return version, err
	}

	version.PublicVersionDigits[2], err = strconv.Atoi(matches[3])
	if err != nil {
		return version, err
	}

	version.PublicVersionDigits[3], err = strconv.Atoi(matches[4])
	if err != nil {
		return version, err
	}

	if len(matches) > 5 && matches[5] != "" {
		version.BuildNum, err = strconv.Atoi(matches[5])
		if err != nil {
			return version, err
		}
	}

	if len(matches) > 6 && matches[6] != "" {
		version.Remainder = matches[6]
	}

	return version, nil

}

func (ybv YBVersion) String() string {
	reprStr, _ := json.Marshal(ybv)
	return string(reprStr)
}

func IsValidVersion(fullVersion string) bool {
	_, err := NewYBVersion(fullVersion)
	return err == nil
}

// returns true if version1 < version2
func LessVersions(version1, version2 string) bool {
	ybversion1, err := NewYBVersion(version1)
	if err != nil {
		panic(err)
	}

	ybversion2, err := NewYBVersion(version2)
	if err != nil {
		panic(err)
	}

	for i := 0; i < 4; i++ {
		if ybversion1.PublicVersionDigits[i] != ybversion2.PublicVersionDigits[i] {
			return ybversion1.PublicVersionDigits[i] < ybversion2.PublicVersionDigits[i]
		}
	}
	if ybversion1.BuildNum != ybversion2.BuildNum {
		return ybversion1.BuildNum < ybversion2.BuildNum
	}

	return ybversion1.Remainder < ybversion2.Remainder
}

// only keep elements which eval to true on filter func
func FilterList[T any](sourceList []T, filterFunc func(T) bool) (result []T) {
	for _, s := range sourceList {
		if filterFunc(s) {
			result = append(result, s)
		}
	}
	return
}

func GetJsonRepr[T any](obj T) []byte {
	reprStr, _ := json.Marshal(obj)
	return reprStr
}

func init() {
	InitViper()
	// Init globals that rely on viper

	/*
		Version = GetVersion()
		InstallRoot = GetInstallRoot()
		InstallVersionDir = GetInstallVersionDir()
		yugabundleBinary = "yugabundle-" + Version + "-centos-x86_64.tar.gz"
		currentUser = GetCurrentUser()
	*/
}

func setYamlValue(filePath string, yamlPath string, value string) {
	origYamlBytes, err := os.ReadFile(filePath)
	if err != nil {
		log.Fatal("unable to read config file " + filePath)
	}

	var root yaml.Node
	err = yaml.Unmarshal(origYamlBytes, &root)
	if err != nil {
		log.Fatal("unable to parse config file " + filePath)
	}

	yPath, err := yamlpath.NewPath(yamlPath)
	if err != nil {
		log.Fatal(fmt.Sprintf("malformed yaml path %s", yamlPath))
	}

	matchNodes, err := yPath.Find(&root)
	if len(matchNodes) != 1 {
		log.Fatal(fmt.Sprintf("yamlPath %s is not accurate", yamlPath))
	}
	matchNodes[0].Value = value

	finalYaml, err := yaml.Marshal(&root)
	if err != nil {
		log.Fatal(fmt.Sprintf("error serializing yaml"))
	}
	err = os.WriteFile(filePath, finalYaml, 0600)
	if err != nil {
		log.Fatal(fmt.Sprintf("error writing file %s", filePath))
	}
}

func generateRandomBytes(n int) ([]byte, error) {

	b := make([]byte, n)
	_, err := rand.Read(b)
	if err != nil {
		return nil, err
	}
	return b, nil
}

// generateRandomStringURLSafe is used to generate random passwords.
func GenerateRandomStringURLSafe(n int) string {

	b, _ := generateRandomBytes(n)
	return base64.URLEncoding.EncodeToString(b)
}

func GuessPrimaryIP() string {
	conn, err := net.Dial("udp", "8.8.8.8:80")
	if err != nil {
		panic(err)
	}
	defer conn.Close()

	localAddr := conn.LocalAddr().(*net.UDPAddr)
	return localAddr.IP.String()
}
