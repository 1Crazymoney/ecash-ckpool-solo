<?php
#
function stnum($num)
{
 $b4 = '';
 $af = '';
 $fmt = number_format($num, 0);
 if ($num > 99999999)
	$b4 = '<span class=urg>';
 else if ($num > 9999999)
	$b4 = '<span class=warn>';
 if ($b4 != '')
	$af = '</span>';
 return $b4.$fmt.$af;
}
#
function dopplns($data, $user)
{
 global $send_sep;

 $pg = '<h1>CKPool</h1>';

 $blk = getparam('blk', true);
 if (nuem($blk))
 {
	$pg = "<br>
<form action=index.php method=POST>
<input type=hidden name=k value=pplns>
Block: <input type=text name=blk size=10 value=''>
&nbsp;<input type=submit name=Calc value=Calc>
</form>";
 }
 else
 {
	$msg = msgEncode('pplns', 'pplns', array('height' => $blk, 'allow_aged' => 'Y'));
	$rep = sendsockreply('pplns', $msg);
	if ($rep == false)
		$ans = array();
	else
		$ans = repDecode($rep);


	if ($ans['ERROR'] != null)
		return '<font color=red size=+1><br>'.$ans['STATUS'].': '.$ans['ERROR'].'</font>';

	$data = array(	'Block' => 'block',
			'Block Hash' => 'block_hash',
			'Block Reward (Satoshis)' => 'block_reward',
			'PPLNS Wanted' => 'diff_want',
			'PPLNS Used' => 'diffacc_total',
			'Elapsed Seconds' => 'pplns_elapsed',
			'Users' => 'rows',
			'Oldest Workinfoid' => 'begin_workinfoid',
			'Oldest Time' => 'begin_stamp',
			'Oldest Epoch' => 'begin_epoch',
			'Block Workinfoid' => 'block_workinfoid',
			'Block Time' => 'block_stamp',
			'Block Epoch' => 'block_epoch',
			'Newest Workinfoid' => 'end_workinfoid',
			'Newest Share Time' => 'end_stamp',
			'Newest Share Epoch' => 'end_epoch',
			'Network Difficulty' => 'block_ndiff',
			'PPLNS Factor' => 'diff_times',
			'PPLNS Added' => 'diff_add',
			'Share Count' => 'share_count');

	$pg = '<br><a href=https://blockchain.info/block-height/';
	$pg .= $ans['block'].'>Blockchain '.$ans['block']."</a><br>\n";
	$pg .= "<br><table callpadding=0 cellspacing=0 border=0>\n";
	$pg .= '<tr class=title>';
	$pg .= '<td class=dl>Name</td>';
	$pg .= '<td class=dr>Value</td>';
	$pg .= "</tr>\n";
	$i = 0;
	foreach ($data as $dsp => $name)
	{
		if (($i++ % 2) == 0)
			$row = 'even';
		else
			$row = 'odd';

		$pg .= "<tr class=$row>";
		$pg .= "<td class=dl>$dsp</td>";
		$pg .= '<td class=dr>'.$ans[$name].'</td>';
		$pg .= "</tr>\n";
	}

	$pg .= "</table><br><table callpadding=0 cellspacing=0 border=0>\n";
	$pg .= '<tr class=title>';
	$pg .= '<td class=dl>User</td>';
	$pg .= '<td class=dr>Diff Accepted</td>';
	$pg .= '<td class=dr>%</td>';
	$pg .= '<td class=dr>Base BTC</td>';
	$pg .= "</tr>\n";

	$diffacc_total = $ans['diffacc_total'];
	if ($diffacc_total == 0)
		$diffacc_total = pow(10,15);
	$reward = 1.0 * $ans['block_reward'] / pow(10,8);
	$count = $ans['rows'];
	for ($i = 1; $i <= $count; $i++)
	{
		$diffacc_user = $ans['diffacc_user'.$i];
		$diffacc_percent = number_format(100.0 * $diffacc_user / $diffacc_total, 2).'%';
		$diffacc_btc = number_format($reward * $diffacc_user / $diffacc_total, 8);

		if (($i % 2) == 0)
			$row = 'even';
		else
			$row = 'odd';

		$pg .= "<tr class=$row>";
		$pg .= '<td class=dl>'.$ans['user'.$i].'</td>';
		$pg .= "<td class=dr>$diffacc_user</td>";
		$pg .= "<td class=dr>$diffacc_percent</td>";
		$pg .= "<td class=dr>$diffacc_btc</td>";
		$pg .= "</tr>\n";
	}
	$pg .= "</table>\n";
 }

 return $pg;
}
#
function show_pplns($menu, $name, $user)
{
 gopage(NULL, 'dopplns', $menu, $name, $user);
}
#
?>
