<?php
#
function workmgtuser($data, $user, $err)
{
 $pg = '<h1>Worker Management</h1>';

 if ($err != '')
	$pg .= "<span class=err>$err<br><br></span>";

 $pg .= makeForm('workmgt');
 $pg .= "<table callpadding=0 cellspacing=0 border=0>\n";
 $pg .= '<tr class=title>';
 $pg .= '<td class=dl>Worker Name</td>';
 $pg .= '<td class=dr>Minimum Diff</td>';
 $pg .= '</tr>';

 $ans = getWorkers($user, 'N');

 $offset = 0;
 if ($ans['STATUS'] == 'ok')
 {
	$count = $ans['rows'];
	for ($i = 0; $i < $count; $i++)
	{
		if ((($offset) % 2) == 0)
			$row = 'even';
		else
			$row = 'odd';

		$pg .= "<tr class=$row>";

		$wn = $ans['workername:'.$i];
		$pg .= '<td class=dl>';
		$pg .= "<input type=hidden name='workername:$i' value='$wn'>";
		$pg .= $wn.'</td>';

		$md = $ans['difficultydefault:'.$i];
		$pg .= '<td class=dr>';
		$pg .= "<input type=text size=6 name='difficultydefault:$i' value='$md'>";
		$pg .= "<input type=submit name=OK value=OK>";
		$pg .= "</td>";

		$pg .= "</tr>\n";

		$offset++;
	}
 }
 $pg .= '<tr><td colspan=2 class=dc><font size=-1><span class=st1>*</span>';
 $pg .= ' A value of 0, less than the pool default,<br>';
 $pg .= 'or less than the pool calculated value for you,<br>';
 $pg .= 'will use the pool calculated value</font></td></tr>';
 $pg .= "</table><input type=hidden name=rows value=$count></form>\n";

 return $pg;
}
#
function doworkmgt($data, $user)
{
 $err = '';
 $OK = getparam('OK', false);
 $count = getparam('rows', false);
 if ($OK == 'OK' && !nuem($count))
 {
	if ($count > 0 && $count < 9999)
	{
		$settings = array();
		for ($i = 0; $i < $count; $i++)
		{
			$wn = getparam('workername:'.$i, false);
			$md = getparam('difficultydefault:'.$i, false);
			if (!nuem($wn) && !nuem($md))
			{
				$settings['workername:'.$i] = $wn;
				$settings['difficultydefault:'.$i] = $md;
			}
		}
		$ans = workerSet($user, $settings);
		if ($ans['STATUS'] != 'ok')
			$err = $ans['ERROR'];
	}
 }

 $pg = workmgtuser($data, $user, $err);

 return $pg;
}
#
function show_workmgt($page, $menu, $name, $user)
{
 gopage(NULL, 'doworkmgt', $page, $menu, $name, $user);
}
#
?>
