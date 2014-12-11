<?php
#
function no_api($json = "")
{
 if (nuem($json))
	 echo 'nil';
 else
	 echo '{"nil":"0"}';
 exit(0);
}
#
function show_api($info, $page, $menu, $name, $user)
{
 global $fld_sep;
 $u = getparam('username', true);
 if (nutem($u))
	no_api();
 $api = getparam('api', true);
 if (nutem($api))
	no_api();
 $jfu = getparam('json', true);
 $work = getparam('work', true);
 $ans = getAtts($u, 'KAPIKey.str');
 if ($ans['STATUS'] != 'ok')
	no_api($jfu);
 if (!isset($ans['KAPIKey.str']))
	no_api($jfu);
 if ($ans['KAPIKey.str'] != $api)
	no_api($jfu);
 if (nuem($work))
 {
	if ($info === NULL)
		$info = homeInfo($u);
	if ($info === false)
		no_api($jfu);
	$rep = fldEncode($info, 'lastbc', true);
	$rep .= fldEncode($info, 'lastheight', false);
	$rep .= fldEncode($info, 'currndiff', false);
	$rep .= fldEncode($info, 'lastblock', false);
	$rep .= fldEncode($info, 'lastblockheight', false);
	$rep .= fldEncode($info, 'blockacc', false);
	$rep .= fldEncode($info, 'blockerr', false);
	$rep .= fldEncode($info, 'p_hashrate5m', false);
	$rep .= fldEncode($info, 'p_hashrate1hr', false);
	$rep .= fldEncode($info, 'u_hashrate5m', false);
	$rep .= fldEncode($info, 'u_hashrate1hr', false);
 }
 else
 {
	$ans = getWorkers($u);
	if ($ans === false)
		no_api($jfu);
	$rep .= fldEncode($ans, 'rows', true);
	$rows = $ans['rows'];
	$flds = explode(',', $ans['flds']);
	$zeflds = '';
	for ($i = 0; $i < $rows; $i++)
		foreach ($flds as $fld)
			if (substr($fld, 0, 7) != 'idlenot')
			{
				$rep .= fldEncode($ans, $fld.':'.$i, false);
				if ($i == 0)
					$zeflds .= "$fld,";
			}
	$rep .= fldEncode($ans, 'arn', false);
	$rep .= fldEncode($ans, 'arp', false);
	$rep .= fldEncode(array(), 'flds', false);
	$rep .= substr($zeflds, 0, -1);
 }
 if (nuem($jfu))
	echo $rep;
 else
 {
	$j = preg_replace("/([^=]+)=([^$fld_sep]*)$fld_sep/", '"$1":"$2",', $rep.$fld_sep);
	echo '{'.substr($j, 0, -1).'}';
 }
 exit(0);
}
#
?>
