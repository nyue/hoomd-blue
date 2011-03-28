#!/bin/bash
# This script facilitates automatic nightly builds of RPM hoomd packages.
# To simply build a package, consider using 'make rpm' instead, or refer to
# the README or spec file for more information.
#
# To perform automatic package builds, this script should be invoked from cron
# with the following additions to crontab:
# MAILTO=''
# 0 0 * * * $HOME/packaging/redhat_build_package.sh >> $HOME/redhat_package_build.out 2>&1
#
# If this script is invoked automatically (as via crontab) from $HOME/packaging/
# then updates in hoomd-blue trunk will take two subsequent runs to take effect.
#
# Use the -f flag to prevent the script from performing an automatic update
# of itself and the Makefile.
#
PATH=/bin:/usr/bin:$PATH
# $0 can't be relied upon to identify the name of this script...
ME=redhat_build_package.sh
SPECFILE="hoomd.spec"
# number of RPMs to retain
NRPMS=8
# what architecture does rpm think we have?
ARCH=`rpm --eval '%{_arch}'`

QUIET='QUIET=true'
UPDATE="true"
while getopts fv OPT $@; do
 case $OPT in
	'f') unset UPDATE ;;
	'v') unset QUIET ;;
 esac
done

echo "$0 running at "`date`

mkdir -p $HOME/packaging
cd $HOME/packaging
if [ "$UPDATE" ]; then
  if ( svn checkout http://codeblue.engin.umich.edu/hoomd-blue/svn/trunk/packaging . | grep -q $ME ) ; then
	echo "This script had changed in source and has been updated."
	echo "Re-exec'ing to avoid problems..."
	sleep 10 # give us a chance to kill this script in case we've screwed up
	exec $HOME/packaging/redhat_build_package.sh
  fi
fi

#check the previous version built
atrev=`cat $HOME/rh_old_revsion || echo 0`
new_rev=`svn info http://codeblue.engin.umich.edu/hoomd-blue/svn/trunk | grep "Last Changed Rev" | awk '{print $NF}'`
echo "Last revision built was $atrev"
echo "Current repository revision is $new_rev"

if [ "$atrev" =  "$new_rev" ];then
	echo "up to date"
else
	echo "commence building"
	# maybe some of this should be moved to cmake
	mkdir -p $HOME/nightly-build
	cp Makefile $HOME/nightly-build/
	cd $HOME/nightly-build
	svn checkout http://codeblue.engin.umich.edu/hoomd-blue/svn/trunk/packaging/SPECS
	make rpm $QUIET || exit
	#set the version we just built in rh_old_revsion so it won't be built again
	echo $new_rev > $HOME/rh_old_revsion
	#move files to be uploaded
	destination="devel/incoming/"`/bin/cat /etc/redhat-release | /usr/bin/awk '{print $1}' | tr '[:upper:]' '[:lower:]'`
	rsync -ue /usr/bin/ssh rpmbuild/RPMS/$ARCH/hoomd*.rpm joaander@foxx.engin.umich.edu:$destination/
fi

#clean up
cd $HOME/rpmbuild/RPMS/$ARCH
rpmfiles=( `ls -td hoomd*.rpm` )
numfiles=${#rpmfiles[*]}
for ((  i=$(( $NRPMS )); $i < $numfiles ; i++ )); do
	rm ${rpmfiles[$i]};
done