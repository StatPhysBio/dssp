//           Copyright Maarten L. Hekkelman 2020
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <exception>
#include <iostream>
#include <fstream>

#include <boost/format.hpp>
#include <boost/date_time/gregorian/formatters.hpp>

#include <cif++/Config.hpp>
#include <cif++/Structure.hpp>
#include <cif++/Secondary.hpp>
#include <cif++/CifUtils.hpp>
#include <cif++/Cif2PDB.hpp>
#include <cif++/FixDMC.hpp>

#include <zeep/streambuf.hpp>

#include <boost/program_options.hpp>

namespace po = boost::program_options;

// --------------------------------------------------------------------

// recursively print exception whats:
void print_what (const std::exception& e)
{
	std::cerr << e.what() << std::endl;
	try
	{
		std::rethrow_if_nested(e);
	}
	catch (const std::exception& nested)
	{
		std::cerr << " >> ";
		print_what(nested);
	}
}

// --------------------------------------------------------------------

namespace {
	std::string gVersionNr, gVersionDate, VERSION_STRING;
}

void load_version_info()
{
	const std::regex
		rxVersionNr(R"(build-(\d+)-g[0-9a-f]{7}(-dirty)?)"),
		rxVersionDate(R"(Date: +(\d{4}-\d{2}-\d{2}).*)");

	auto version = cif::rsrc_loader::load("version.txt");
	if (not version)
		VERSION_STRING = "unknown version, version resource is missing";
	else
	{
		zeep::char_streambuf buffer(version.data(), version.size());
		std::istream is(&buffer);
		std::string line;

		while (getline(is, line))
		{
			std::smatch m;

			if (std::regex_match(line, m, rxVersionNr))
			{
				gVersionNr = m[1];
				if (m[2].matched)
					gVersionNr += '*';
				continue;
			}

			if (regex_match(line, m, rxVersionDate))
			{
				gVersionDate = m[1];
				continue;
			}
		}

		if (not VERSION_STRING.empty())
			VERSION_STRING += "\n";
		VERSION_STRING += gVersionNr + " " + gVersionDate;
	}
}

std::string get_version_nr()
{
	return gVersionNr;
}

std::string get_version_date()
{
	return gVersionDate;
}

// --------------------------------------------------------------------

std::string ResidueToDSSPLine(const mmcif::DSSP::ResidueInfo& info)
{
/*
	This is the header line for the residue lines in a DSSP file:

	#  RESIDUE AA STRUCTURE BP1 BP2  ACC     N-H-->O    O-->H-N    N-H-->O    O-->H-N    TCO  KAPPA ALPHA  PHI   PSI    X-CA   Y-CA   Z-CA
 */
	boost::format kDSSPResidueLine(
		"%5.5d%5.5d%1.1s%1.1s %c  %c%c%c%c%c%c%c%c%c%4.4d%4.4d%c%4.4d %11s%11s%11s%11s  %6.3f%6.1f%6.1f%6.1f%6.1f %6.1f %6.1f %6.1f");

	auto& residue = info.residue();

	if (residue.asymID().length() > 1)
		throw std::runtime_error("This file contains data that won't fit in the original DSSP format");

	char code = 'X';
	if (mmcif::kAAMap.find(residue.compoundID()) != mmcif::kAAMap.end())
		code = mmcif::kAAMap.at(residue.compoundID());

	if (code == 'C')	// a cysteine
	{
		auto ssbridgenr = info.ssBridgeNr();
		if (ssbridgenr)
			code = 'a' + ((ssbridgenr - 1) % 26);
	}

	char ss;
	switch (info.ss())
	{
		case mmcif::ssAlphahelix:	ss = 'H'; break;
		case mmcif::ssBetabridge:	ss = 'B'; break;
		case mmcif::ssStrand:		ss = 'E'; break;
		case mmcif::ssHelix_3:		ss = 'G'; break;
		case mmcif::ssHelix_5:		ss = 'I'; break;
		case mmcif::ssHelix_PPII:		ss = 'P'; break;
		case mmcif::ssTurn:			ss = 'T'; break;
		case mmcif::ssBend:			ss = 'S'; break;
		case mmcif::ssLoop:			ss = ' '; break;
	}

	char helix[4] = { ' ', ' ', ' ', ' ' };
	for (mmcif::HelixType helixType: { mmcif::HelixType::rh_3_10, mmcif::HelixType::rh_alpha, mmcif::HelixType::rh_pi, mmcif::HelixType::rh_pp })
	{
		switch (info.helix(helixType))
		{
			case mmcif::Helix::None:		helix[static_cast<int>(helixType)] = ' '; break;
			case mmcif::Helix::Start:		helix[static_cast<int>(helixType)] = '>'; break;
			case mmcif::Helix::End:			helix[static_cast<int>(helixType)] = '<'; break;
			case mmcif::Helix::StartAndEnd:	helix[static_cast<int>(helixType)] = 'X'; break;
			case mmcif::Helix::Middle:		helix[static_cast<int>(helixType)] = (helixType == mmcif::HelixType::rh_pp ? 'P' : ('3' + static_cast<int>(helixType))); break;
		}
	}

	char bend = ' ';
	if (info.bend())
		bend = 'S';

	double alpha = residue.alpha();
	char chirality = alpha == 360 ? ' ' : (alpha < 0 ? '-' : '+');
	
	uint32_t bp[2] = {};
	char bridgelabel[2] = { ' ', ' ' };
	for (uint32_t i: { 0, 1 })
	{
		const auto& [p, ladder, parallel] = info.bridgePartner(i);
		if (not p)
			continue;

		bp[i] = p.nr() % 10000;  // won't fit otherwise...
		bridgelabel[i] = (parallel ? 'a' : 'A') + ladder % 26;
	}

	char sheet = ' ';
	if (info.sheet() != 0)
		sheet = 'A' + (info.sheet() - 1) % 26;

	std::string NHO[2], ONH[2];
	for (int i: { 0, 1 })
	{
		const auto& [donor, donorE] = info.donor(i);
		const auto& [acceptor, acceptorE] = info.acceptor(i);
		
		NHO[i] = ONH[i] = "0, 0.0";

		if (acceptor)
		{
			auto d = acceptor.nr() - info.nr();
			NHO[i] = (boost::format("%d,%3.1f") % d % acceptorE).str();
		}

		if (donor)
		{
			auto d = donor.nr() - info.nr();
			ONH[i] = (boost::format("%d,%3.1f") % d % donorE).str();
		}
	}

	auto ca = residue.atomByID("CA");
	auto const& [cax, cay, caz] = ca.location();

	return (kDSSPResidueLine % info.nr() % ca.authSeqID() % ca.pdbxAuthInsCode() % ca.authAsymID() % code %
		ss % helix[3] % helix[0] % helix[1] % helix[2] % bend % chirality % bridgelabel[0] % bridgelabel[1] %
		bp[0] % bp[1] % sheet % floor(info.accessibility() + 0.5) %
		NHO[0] % ONH[0] % NHO[1] % ONH[1] %
		residue.tco() % residue.kappa() % alpha % residue.phi() % residue.psi() %
		cax % cay % caz).str();
}

void writeDSSP(const mmcif::Structure& structure, const mmcif::DSSP& dssp, std::ostream& os)
{
	const std::string kFirstLine("==== Secondary Structure Definition by the program DSSP, NKI version 3.0                           ==== ");
	boost::format kHeaderLine("%1% %|127t|%2%");

	using namespace boost::gregorian;

	auto stats = dssp.GetStatistics();

	date today = day_clock::local_day();

	auto& cf = structure.getFile().file();

	os << kHeaderLine % (kFirstLine + "DATE=" + to_iso_extended_string(today)) % '.' << std::endl
	   << kHeaderLine % "REFERENCE W. KABSCH AND C.SANDER, BIOPOLYMERS 22 (1983) 2577-2637" % '.' << std::endl
	   << GetPDBHEADERLine(cf, 127) << '.' << std::endl
	   << GetPDBCOMPNDLine(cf, 127) << '.' << std::endl
	   << GetPDBSOURCELine(cf, 127) << '.' << std::endl
	   << GetPDBAUTHORLine(cf, 127) << '.' << std::endl;

	os << boost::format("%5.5d%3.3d%3.3d%3.3d%3.3d TOTAL NUMBER OF RESIDUES, NUMBER OF CHAINS, NUMBER OF SS-BRIDGES(TOTAL,INTRACHAIN,INTERCHAIN) %|127t|%c") %
			 stats.nrOfResidues % stats.nrOfChains % stats.nrOfSSBridges % stats.nrOfIntraChainSSBridges % (stats.nrOfSSBridges - stats.nrOfIntraChainSSBridges) % '.' << std::endl;
		 os << kHeaderLine % (boost::format("%8.1f   ACCESSIBLE SURFACE OF PROTEIN (ANGSTROM**2)") % stats.accessibleSurface) % '.' << std::endl;

	// hydrogenbond summary

	os << kHeaderLine % (
		boost::format("%5.5d%5.1f   TOTAL NUMBER OF HYDROGEN BONDS OF TYPE O(I)-->H-N(J)  , SAME NUMBER PER 100 RESIDUES")
			% stats.nrOfHBonds % (stats.nrOfHBonds * 100.0 / stats.nrOfResidues)) % '.' << std::endl;

	os << kHeaderLine % (
		boost::format("%5.5d%5.1f   TOTAL NUMBER OF HYDROGEN BONDS IN     PARALLEL BRIDGES, SAME NUMBER PER 100 RESIDUES")
			% stats.nrOfHBondsInParallelBridges % (stats.nrOfHBondsInParallelBridges * 100.0 / stats.nrOfResidues)) % '.' << std::endl;

	os << kHeaderLine % (
		boost::format("%5.5d%5.1f   TOTAL NUMBER OF HYDROGEN BONDS IN ANTIPARALLEL BRIDGES, SAME NUMBER PER 100 RESIDUES")
			% stats.nrOfHBondsInAntiparallelBridges % (stats.nrOfHBondsInAntiparallelBridges * 100.0 / stats.nrOfResidues)) % '.' << std::endl;

	boost::format kHBondsLine("%5.5d%5.1f   TOTAL NUMBER OF HYDROGEN BONDS OF TYPE O(I)-->H-N(I%c%1.1d), SAME NUMBER PER 100 RESIDUES");
	for (int k = 0; k < 11; ++k)
		os << kHeaderLine % (kHBondsLine % stats.nrOfHBondsPerDistance[k] % (stats.nrOfHBondsPerDistance[k] * 100.0 / stats.nrOfResidues) % (k - 5 < 0 ? '-' : '+') % abs(k - 5)) % '.' << std::endl;

	// histograms...
	os << "  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30     *** HISTOGRAMS OF ***           ." << std::endl;

	for (auto hi: stats.residuesPerAlphaHelixHistogram)
		os << boost::format("%3.3d") % hi;
	os << "    RESIDUES PER ALPHA HELIX         ." << std::endl;

	for (auto hi: stats.parallelBridgesPerLadderHistogram)
		os << boost::format("%3.3d") % hi;
	os << "    PARALLEL BRIDGES PER LADDER      ." << std::endl;

	for (auto hi: stats.antiparallelBridgesPerLadderHistogram)
		os << boost::format("%3.3d") % hi;
	os << "    ANTIPARALLEL BRIDGES PER LADDER  ." << std::endl;

	for (auto hi: stats.laddersPerSheetHistogram)
		os << boost::format("%3.3d") % hi;
	os << "    LADDERS PER SHEET                ." << std::endl;

	// per residue information

	os << "  #  RESIDUE AA STRUCTURE BP1 BP2  ACC     N-H-->O    O-->H-N    N-H-->O    O-->H-N    TCO  KAPPA ALPHA  PHI   PSI    X-CA   Y-CA   Z-CA" << std::endl;
	boost::format kDSSPResidueLine(
		"%5.5d        !%c             0   0    0      0, 0.0     0, 0.0     0, 0.0     0, 0.0   0.000 360.0 360.0 360.0 360.0    0.0    0.0    0.0");

	int last = 0;
	for (auto ri: dssp)
	{
		// insert a break line whenever we detect missing residues
		// can be the transition to a different chain, or missing residues in the current chain

		if (ri.nr() != last + 1)
			os << (kDSSPResidueLine % (last + 1) % (ri.chainBreak() == mmcif::ChainBreak::NewChain ? '*' : ' ')) << std::endl;
		
		os << ResidueToDSSPLine(ri) << std::endl;
		last = ri.nr();
	}

	// std::vector<const MResidue*> residues;

	// foreach (const MChain* chain, protein.GetChains())
	// {
	// 	foreach (const MResidue* residue, chain->GetResidues())
	// 		residues.push_back(residue);
	// }

	// // keep residues sorted by residue number as assigned during reading the PDB file
	// sort(residues.begin(), residues.end(), boost::bind(&MResidue::GetNumber, _1) < boost::bind(&MResidue::GetNumber, _2));

	// const MResidue* last = nullptr;
	// foreach (const MResidue* residue, residues)
	// {
	// 	// insert a break line whenever we detect missing residues
	// 	// can be the transition to a different chain, or missing residues in the current chain
	// 	if (last != nullptr and last->GetNumber() + 1 != residue->GetNumber())
	// 	{
	// 		char breaktype = ' ';
	// 		if (last->GetChainID() != residue->GetChainID())
	// 			breaktype = '*';
	// 		os << (kDSSPResidueLine % (last->GetNumber() + 1) % breaktype) << std::endl;
	// 	}
	// 	os << ResidueToDSSPLine(*residue) << std::endl;
	// 	last = residue;
	// }
}

void annotateDSSP(mmcif::Structure& structure, const mmcif::DSSP& dssp, std::ostream& os)
{
	auto& db = structure.getFile().data();

	if (dssp.empty())
	{
		if (cif::VERBOSE)
			std::cout << "No secondary structure information found" << std::endl;
	}
	else
	{
		// replace all struct_conf and struct_conf_type records
		auto& structConfType = db["struct_conf_type"];
		structConfType.clear();

		auto& structConf = db["struct_conf"];
		structConf.clear();

		std::map<std::string,int> foundTypes;

		auto st = dssp.begin(), lt = st;
		auto lastSS = st->ss();
		std::string id;

		for (auto t = dssp.begin(); ; lt = t, ++t)
		{
			bool stop = t == dssp.end();

			bool flush = (stop or t->ss() != lastSS);

			if (flush and lastSS != mmcif::SecondaryStructureType::ssLoop)
			{
				auto& rb = st->residue();
				auto& re = lt->residue();

				structConf.emplace({
					{ "conf_type_id", id },
					{ "id", id + std::to_string(foundTypes[id]++) },
					// { "pdbx_PDB_helix_id", vS(12, 14) },
					{ "beg_label_comp_id", rb.compoundID() },
					{ "beg_label_asym_id", rb.asymID() },
					{ "beg_label_seq_id", rb.seqID() },
					{ "pdbx_beg_PDB_ins_code", rb.authInsCode() },
					{ "end_label_comp_id", re.compoundID() },
					{ "end_label_asym_id", re.asymID() },
					{ "end_label_seq_id", re.seqID() },
					{ "pdbx_end_PDB_ins_code", re.authInsCode() },
		
					{ "beg_auth_comp_id", rb.compoundID() },
					{ "beg_auth_asym_id", rb.authAsymID() },
					{ "beg_auth_seq_id", rb.authSeqID() },
					{ "end_auth_comp_id", re.compoundID() },
					{ "end_auth_asym_id", re.authAsymID() },
					{ "end_auth_seq_id", re.authSeqID() },
		
					{ "criteria", "DSSP" }

					// { "pdbx_PDB_helix_class", vS(39, 40) },
					// { "details", vS(41, 70) },
					// { "pdbx_PDB_helix_length", vI(72, 76) }
				});

				st = t;
			}

			if (lastSS != t->ss())
			{
				st = t;
				lastSS = t->ss();
			}

			if (stop)
				break;

			if (not flush)
				continue;

			switch (t->ss())
			{
				case mmcif::SecondaryStructureType::ssHelix_3:
					id = "HELX_RH_3T_P";
					break;

				case mmcif::SecondaryStructureType::ssAlphahelix:
					id = "HELX_RH_AL_P";
					break;

				case mmcif::SecondaryStructureType::ssHelix_5:
					id = "HELX_RH_PI_P";
					break;

				case mmcif::SecondaryStructureType::ssHelix_PPII:
					id = "HELX_LH_PP_P";
					break;

				case mmcif::SecondaryStructureType::ssTurn:
					id = "TURN_TY1_P";
					break;

				case mmcif::SecondaryStructureType::ssBend:
					id = "TURN_P";
					break;

				case mmcif::SecondaryStructureType::ssBetabridge:
				case mmcif::SecondaryStructureType::ssStrand:
					id = "STRN";
					break;

				default:
					id.clear();
					break;
			}

			if (id.empty())
				continue;

			if (foundTypes.count(id) == 0)
			{
				structConfType.emplace({
					{ "id", id }
				});
				foundTypes[id] = 1;
			}
		}
	}

	db.add_software("dssp " VERSION, "other", get_version_nr(), get_version_date());

	db.write(os);

// 	cif::File df;
	
// 	df.append(new cif::Datablock("DSSP_" + structure.getFile().data().getName()));

// 	auto& db = df.firstDatablock();

// 	int last = 0;
// 	for (auto info: dssp)
// 	{
// 		// insert a break line whenever we detect missing residues
// 		// can be the transition to a different chain, or missing residues in the current chain


// 		// 	os << (kDSSPResidueLine % (last + 1) % (ri.chainBreak() == mmcif::ChainBreak::NewChain ? '*' : ' ')) << std::endl;
		
// 		// os << ResidueToDSSPLine(ri) << std::endl;

// 		auto& mon = info.residue();

// 		auto&& [row, rn] = db["struct_mon_prot"].emplace({
// 			{ "chain_break", (info.chainBreak() == mmcif::ChainBreak::Gap ? 'Y' : '.') },
// 			{ "label_comp_id", mon.compoundID() },
// 			{ "label_asym_id", mon.asymID() },
// 			{ "label_seq_id", mon.seqID() },
// 			{ "label_alt_id", info.alt_id() },
// 			{ "auth_asym_id", mon.authAsymID() },
// 			{ "auth_seq_id", mon.authSeqID() },
// 			{ "auth_ins_code", mon.authInsCode() },
// 		});

// 		if (mon.is_first_in_chain())
// 			row["phi"] = ".";
// 		else
// 			row["phi"].os(std::fixed, std::setprecision(1), mon.phi());

// 		if (mon.is_last_in_chain())
// 			row["psi"] = ".";
// 		else
// 			row["psi"].os(std::fixed, std::setprecision(1), mon.psi());

// 		if (mon.is_last_in_chain())
// 			row["omega"] = ".";
// 		else
// 			row["omega"].os(std::fixed, std::setprecision(1), mon.omega());

// 		int nrOfChis = mon.nrOfChis();
// 		for (int i = 0; i < 5; ++i)
// 		{
// 			auto cl = "chi" + std::to_string(i + 1);
// 			if (i < nrOfChis)
// 				row[cl].os(std::fixed, std::setprecision(1), mon.chi(i));
// 			else
// 				row[cl] = '.';
// 		}

// 		if (not mon.has_alpha())
// 			row["alpha"] = ".";
// 		else
// 			row["alpha"].os(std::fixed, std::setprecision(1), mon.alpha());

// 		if (not mon.has_kappa())
// 			row["kappa"] = ".";
// 		else
// 			row["kappa"].os(std::fixed, std::setprecision(1), mon.kappa());

// 		if (mon.is_first_in_chain())
// 			row["tco"] = ".";
// 		else
// 			row["tco"].os(std::fixed, std::setprecision(1), mon.tco());

// 		// sec structure info

// 		char ss;
// 		switch (info.ss())
// 		{
// 			case mmcif::ssAlphahelix:	ss = 'H'; break;
// 			case mmcif::ssBetabridge:	ss = 'B'; break;
// 			case mmcif::ssStrand:		ss = 'E'; break;
// 			case mmcif::ssHelix_3:		ss = 'G'; break;
// 			case mmcif::ssHelix_5:		ss = 'I'; break;
// 			case mmcif::ssHelix_PPII:		ss = 'P'; break;
// 			case mmcif::ssTurn:			ss = 'T'; break;
// 			case mmcif::ssBend:			ss = 'S'; break;
// 			case mmcif::ssLoop:			ss = '.'; break;
// 		}
// 		row["dssp_symbol"] = ss;

// 		if (mon.has_alpha())
// 			row["Calpha_chiral_sign"] = mon.alpha() < 0 ? "neg" : "pos";
// 		else
// 			row["Calpha_chiral_sign"] = ".";

// 		row["sheet_id"] = info.sheet() ? std::to_string(info.sheet()) : ".";

// 		for (uint32_t i: { 0, 1 })
// 		{
// 			std::string il = "bridge_partner_" + std::to_string(i + 1);

// 			const auto& [p, ladder, parallel] = info.bridgePartner(i);
// 			if (not p)
// 			{
// 				row[il + "_label_comp_id"] = ".";
// 				row[il + "_label_asym_id"] = ".";
// 				row[il + "_label_seq_id"] = ".";
// 				row[il + "_auth_asym_id"] = ".";
// 				row[il + "_auth_seq_id"] = ".";
// 				row[il + "_ladder"] = ".";
// 				row[il + "_sense"] = ".";
// 				continue;
// 			}

// 			auto& pm = p.residue();

// 			row[il + "_label_comp_id"] = pm.compoundID();
// 			row[il + "_label_asym_id"] = pm.asymID();
// 			row[il + "_label_seq_id"] = pm.seqID();
// 			row[il + "_auth_asym_id"] = pm.authAsymID();
// 			row[il + "_auth_seq_id"] = pm.authSeqID();
// 			row[il + "_ladder"] = ladder;
// 			row[il + "_sense"] = parallel ? "parallel" : "anti-parallel";
// 		}

// 		for (auto stride: { 3, 4, 5})
// 		{
// 			std::string hs = "helix_info_" + std::to_string(stride);
// 			switch (info.helix(stride))
// 			{
// #if 0
// 				case mmcif::Helix::None:		row[hs] = '.'; break;
// 				case mmcif::Helix::Start:		row[hs] = "start"; break;
// 				case mmcif::Helix::End:			row[hs] = "end"; break;
// 				case mmcif::Helix::StartAndEnd:	row[hs] = "start-and-end"; break;
// 				case mmcif::Helix::Middle:		row[hs] = "middle"; break;
// #else
// 				case mmcif::Helix::None:		row[hs] = '.'; break;
// 				case mmcif::Helix::Start:		row[hs] = '>'; break;
// 				case mmcif::Helix::End:			row[hs] = '<'; break;
// 				case mmcif::Helix::StartAndEnd:	row[hs] = 'X'; break;
// 				case mmcif::Helix::Middle:		row[hs] = '0' + stride; break;
// #endif
// 			}
// 		}

// 		if (info.bend())
// 			row["bend"] = 'S';
// 		else
// 			row["bend"] = ".";

// 		for (int i: { 0, 1 })
// 		{
// 			const auto& [donor, donorE] = info.donor(i);
// 			const auto& [acceptor, acceptorE] = info.acceptor(i);

// 			std::string ds = "O_donor_" + std::to_string(i + 1);
// 			std::string as = "NH_acceptor_" + std::to_string(i + 1);
			
// 			if (acceptor)
// 			{
// 				auto& am = acceptor.residue();

// 				row[as + "_label_comp_id"] = am.compoundID();
//  				row[as + "_label_asym_id"] = am.asymID();
//  				row[as + "_label_seq_id"] = am.seqID();
//  				row[as + "_auth_asym_id"] = am.authAsymID();
//  				row[as + "_auth_seq_id"] = am.authSeqID();
// 				row[as + "_energy"].os(std::fixed, std::setprecision(2), acceptorE);
// 			}
// 			else
// 			{
// 				row[as + "_label_comp_id"] = ".";
//  				row[as + "_label_asym_id"] = ".";
//  				row[as + "_label_seq_id"] = ".";
//  				row[as + "_auth_asym_id"] = ".";
//  				row[as + "_auth_seq_id"] = ".";
// 				row[as + "_energy"] = ".";
// 			}

// 			if (donor)
// 			{
// 				auto& dm = donor.residue();

// 				row[ds + "_label_comp_id"] = dm.compoundID();
//  				row[ds + "_label_asym_id"] = dm.asymID();
//  				row[ds + "_label_seq_id"] = dm.seqID();
//  				row[ds + "_auth_asym_id"] = dm.authAsymID();
//  				row[ds + "_auth_seq_id"] = dm.authSeqID();
// 				row[ds + "_energy"].os(std::fixed, std::setprecision(2), donorE);
// 			}
// 			else
// 			{
// 				row[ds + "_label_comp_id"] = ".";
//  				row[ds + "_label_asym_id"] = ".";
//  				row[ds + "_label_seq_id"] = ".";
//  				row[ds + "_auth_asym_id"] = ".";
//  				row[ds + "_auth_seq_id"] = ".";
// 				row[ds + "_energy"] = ".";
// 			}
// 		}

// 		last = info.nr();
// 	}


// 	df.write(os, {});
}

// --------------------------------------------------------------------

int d_main(int argc, const char* argv[])
{
	using namespace std::literals;

	po::options_description visible_options(argv[0] + " input-file [output-file] [options]"s);
	visible_options.add_options()
		("xyzin",				po::value<std::string>(),	"coordinates file")
		("output",              po::value<std::string>(),	"Output to this file")

		("dict",				po::value<std::vector<std::string>>(),
															"Dictionary file containing restraints for residues in this specific target, can be specified multiple times.")

		("help,h",											"Display help message")
		("version",											"Print version")

		("output-format",		po::value<std::string>(),	"Output format, can be either 'dssp' for classic DSSP or 'mmcif' for annotated mmCIF. The default is chosen based on the extension of the output file, if any.")

		("create-missing",									"Create missing backbone atoms")

#if not USE_RSRC
		("rsrc-dir",			po::value<std::string>(),	"Directory containing the 'resources' used by this application")
#endif

		("min-pp-stretch",		po::value<short>(),			"Minimal number of residues having PSI/PHI in range for a PP helix, default is 3")

		("verbose,v",										"verbose output")
		;
	
	po::options_description hidden_options("hidden options");
	hidden_options.add_options()
		("debug,d",				po::value<int>(),			"Debug level (for even more verbose output)")
		;

	po::options_description cmdline_options;
	cmdline_options.add(visible_options).add(hidden_options);

	po::positional_options_description p;
	p.add("xyzin", 1);
	p.add("output", 1);
	
	po::variables_map vm;
	po::store(po::command_line_parser(argc, argv).options(cmdline_options).positional(p).run(), vm);

	po::notify(vm);

	// --------------------------------------------------------------------

	if (vm.count("version"))
	{
		std::cout << argv[0] << ' ' << VERSION " version " << VERSION_STRING << std::endl;
		exit(0);
	}

	if (vm.count("help"))
	{
		std::cerr << visible_options << std::endl;
		exit(0);
	}
	
	if (vm.count("xyzin") == 0)
	{
		std::cerr << "Input file not specified" << std::endl;
		exit(1);
	}

	if (vm.count("output-format") and vm["output-format"].as<std::string>() != "dssp" and vm["output-format"].as<std::string>() != "mmcif")
	{
		std::cerr << "Output format should be one of 'dssp' or 'mmcif'" << std::endl;
		exit(1);
	}

	cif::VERBOSE = vm.count("verbose") != 0;
	if (vm.count("debug"))
		cif::VERBOSE = vm["debug"].as<int>();

	// --------------------------------------------------------------------
	
	if (vm.count("dict"))
	{
		for (auto dict: vm["dict"].as<std::vector<std::string>>())
			mmcif::CompoundFactory::instance().pushDictionary(dict);
	}

	mmcif::File f(vm["xyzin"].as<std::string>());
	mmcif::Structure structure(f, 1, mmcif::StructureOpenOptions::SkipHydrogen);

	// --------------------------------------------------------------------
	
	if (vm.count("create-missing"))
		mmcif::CreateMissingBackboneAtoms(structure, true);

	// --------------------------------------------------------------------

	short pp_stretch = 3;
	if (vm.count("min-pp-stretch"))
		pp_stretch = vm["min-pp-stretch"].as<short>();

	mmcif::DSSP dssp(structure, pp_stretch);

	std::string fmt;
	if (vm.count("output-format"))
		fmt = vm["output-format"].as<std::string>();
	
	if (vm.count("output"))
	{
		std::ofstream of(vm["output"].as<std::string>());
		if (not of.is_open())
		{
			std::cerr << "Could not open output file" << std::endl;
			exit(1);
		}
		
		if (fmt == "dssp")
			writeDSSP(structure, dssp, of);
		else
			annotateDSSP(structure, dssp, of);
	}
	else
	{
		if (fmt == "dssp")
			writeDSSP(structure, dssp, std::cout);
		else
			annotateDSSP(structure, dssp, std::cout);
	}
	
	return 0;
}

// --------------------------------------------------------------------

int main(int argc, const char* argv[])
{
	int result = 0;

	try
	{

		cif::rsrc_loader::init({
#if USE_RSRC
			{ cif::rsrc_loader_type::mrsrc, "", { gResourceIndex, gResourceData, gResourceName } },
#endif
			{ cif::rsrc_loader_type::file, "." }
		});

		load_version_info();

		result = d_main(argc, argv);
	}
	catch (const std::exception& ex)
	{
		print_what(ex);
		exit(1);
	}

	return result;
}
